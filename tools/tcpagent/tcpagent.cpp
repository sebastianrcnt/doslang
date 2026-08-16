#include <bios.h>
#include <conio.h>
#include <dos.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "trace.h"
#include "utils.h"
#include "packet.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"
#include "tcpsockm.h"

#define LINE_SIZE 9000
#define CHUNK_SIZE 4096
#define SERVER_PORT 5558
#define LOCAL_PORT 2058
#define RECV_SIZE 12288

static char linebuf[LINE_SIZE];
static unsigned char data[CHUNK_SIZE+1];
static TcpSocket *socketp;
static volatile uint8_t stop_requested;
static FILE *put_file;
static unsigned long put_remaining;

void __interrupt __far ctrl_break(void) { stop_requested=1; }
void __interrupt __far ctrl_c(void) { stop_requested=1; }

static void drive(void) {
    PACKET_PROCESS_MULT(5);
    Arp::driveArp();
    Tcp::drivePackets();
}

static int send_all(const void *buffer, unsigned length) {
    const uint8_t *p=(const uint8_t *)buffer;
    unsigned sent=0;
    int rc;
    while(sent<length && !stop_requested) {
        drive();
        rc=socketp->send((uint8_t *)(p+sent),length-sent);
        if(rc>0) sent+=(unsigned)rc;
        else if(rc<0 || socketp->isRemoteClosed()) return -1;
    }
    return sent==length ? 0 : -1;
}

static void write_text(const char *s) { send_all(s,strlen(s)); }
static int hexval(int c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='A'&&c<='F') return c-'A'+10;
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
static int decode_hex(const char *src,unsigned char *dst,int cap) {
    int n=0,hi,lo;
    while(*src&&src[1]) {
        if(n>=cap) return -1;
        hi=hexval((unsigned char)src[0]); lo=hexval((unsigned char)src[1]);
        if(hi<0||lo<0) return -1;
        dst[n++]=(unsigned char)((hi<<4)|lo); src+=2;
    }
    return *src ? -1 : n;
}
static void write_hex(const unsigned char *src,unsigned count) {
    static const char digits[]="0123456789ABCDEF";
    char out[256]; unsigned i,n;
    while(count) {
        n=count>sizeof(out)/2 ? sizeof(out)/2 : count;
        for(i=0;i<n;++i) { out[i*2]=digits[src[i]>>4]; out[i*2+1]=digits[src[i]&15]; }
        send_all(out,n*2); src+=n; count-=n;
    }
}
static void ok_data(const unsigned char *src,unsigned count) {
    write_text("OK "); write_hex(src,count); write_text("\r\n");
}
static void error_text(const char *s) {
    write_text("ERR "); write_hex((const unsigned char *)s,strlen(s)); write_text("\r\n");
}
static int decode_path(const char *hex,char *path,int cap) {
    int n=decode_hex(hex,(unsigned char *)path,cap-1);
    if(n<0) return 0; path[n]='\0'; return 1;
}
static void command_read(char *args) {
    char path[260],*off=strchr(args,' '); FILE *f; long pos; size_t count; int eof;
    if(!off){error_text("READ requires path and offset");return;} *off++='\0';
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    pos=atol(off); f=fopen(path,"rb"); if(!f){error_text("Cannot open file");return;}
    if(fseek(f,pos,SEEK_SET)){fclose(f);error_text("Cannot seek file");return;}
    count=fread(data,1,CHUNK_SIZE,f); eof=count<CHUNK_SIZE; fclose(f);
    sprintf(linebuf,"OK %d ",eof); write_text(linebuf); write_hex(data,count); write_text("\r\n");
}
static void command_write(char *args) {
    char path[260],*mode=strchr(args,' '),*payload; FILE *f; int count;
    if(!mode){error_text("WRITE requires path, mode and data");return;} *mode++='\0';
    payload=strchr(mode,' '); if(!payload){error_text("WRITE requires data");return;} *payload++='\0';
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    count=decode_hex(payload,data,CHUNK_SIZE); if(count<0){error_text("Invalid data encoding");return;}
    f=fopen(path,mode[0]=='A'?"ab":"wb"); if(!f){error_text("Cannot write file");return;}
    if(count&&fwrite(data,1,count,f)!=(size_t)count){fclose(f);error_text("Short write");return;}
    fclose(f); ok_data((const unsigned char *)"",0);
}
static void command_put(char *args) {
    char path[260],*length_text=strchr(args,' ');
    if(!length_text){error_text("PUT requires path and length");return;}
    *length_text++='\0';
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    put_remaining=strtoul(length_text,0,10);
    put_file=fopen(path,"wb");
    if(!put_file){put_remaining=0;error_text("Cannot write file");return;}
    if(!put_remaining){fclose(put_file);put_file=0;ok_data((const unsigned char *)"",0);}
}
static void command_get(char *args) {
    char path[260]; FILE *f; long length; size_t count;
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    f=fopen(path,"rb"); if(!f){error_text("Cannot open file");return;}
    fseek(f,0,SEEK_END); length=ftell(f); fseek(f,0,SEEK_SET);
    sprintf(linebuf,"DATA %ld\r\n",length); write_text(linebuf);
    while((count=fread(data,1,CHUNK_SIZE,f))>0) if(send_all(data,count)<0)break;
    fclose(f);
}
static void command_hash(char *args) {
    char path[260]; FILE *f; size_t count; unsigned i; unsigned long length=0,hash=2166136261UL;
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    f=fopen(path,"rb"); if(!f){error_text("Cannot open file");return;}
    while((count=fread(data,1,CHUNK_SIZE,f))>0){length+=(unsigned long)count;for(i=0;i<count;++i){hash^=data[i];hash*=16777619UL;}}
    fclose(f); sprintf(linebuf,"STAT %lu %08lX\r\n",length,hash); write_text(linebuf);
}
static void command_list(char *args) {
    char path[260],pattern[300],output[CHUNK_SIZE]; struct find_t found;
    unsigned used=0; int rc;
    if(!decode_path(args,path,sizeof(path))){error_text("Invalid path encoding");return;}
    strcpy(pattern,path); if(pattern[0]&&pattern[strlen(pattern)-1]!='\\') strcat(pattern,"\\"); strcat(pattern,"*.*");
    rc=_dos_findfirst(pattern,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&found);
    while(rc==0) { char entry[100]; int len;
        if(strcmp(found.name,".")&&strcmp(found.name,"..")) { sprintf(entry,"%s\t%lu\t%s\n",found.name,found.size,(found.attrib&_A_SUBDIR)?"DIR":"FILE"); len=strlen(entry); if(used+(unsigned)len>=sizeof(output)) break; memcpy(output+used,entry,len); used+=(unsigned)len; }
        rc=_dos_findnext(&found);
    }
    ok_data((unsigned char *)output,used);
}
static void command_exec(char *args) {
    char command[700],shell[800]; const char *tmp="C:\\PIEXEC.TMP"; FILE *f; int n,code; size_t count;
    n=decode_hex(args,(unsigned char *)command,sizeof(command)-1); if(n<0){error_text("Invalid command encoding");return;} command[n]='\0';
    sprintf(shell,"COMMAND.COM /C %s > %s",command,tmp); code=system(shell);
    f=fopen(tmp,"rb"); count=f?fread(data,1,CHUNK_SIZE,f):0; if(f)fclose(f); remove(tmp);
    sprintf(linebuf,"OK %d ",code); write_text(linebuf); write_hex(data,count); write_text("\r\n");
}
static void process_line(char *line) {
    char *cmd=line,*args=strchr(line,' '); if(args)*args++='\0';else args=cmd+strlen(cmd);
    if(!strcmp(cmd,"PING"))ok_data((const unsigned char *)"PONG",4);
    else if(!strcmp(cmd,"READ"))command_read(args);
    else if(!strcmp(cmd,"WRITE"))command_write(args);
    else if(!strcmp(cmd,"PUT"))command_put(args);
    else if(!strcmp(cmd,"GET"))command_get(args);
    else if(!strcmp(cmd,"HASH"))command_hash(args);
    else if(!strcmp(cmd,"LIST"))command_list(args);
    else if(!strcmp(cmd,"EXEC"))command_exec(args);
    else if(!strcmp(cmd,"QUIT")){ok_data((const unsigned char *)"BYE",3);stop_requested=1;}
    else { char message[96]; sprintf(message,"Unknown command: %.70s",cmd); error_text(message); }
}
static int connect_host(void) {
    IpAddr_t host={10,0,2,2}; int8_t rc;
    socketp=TcpSocketMgr::getSocket(); if(!socketp)return -1;
    socketp->setRecvBuffer(RECV_SIZE);
    rc=socketp->connect(LOCAL_PORT,host,SERVER_PORT,10000);
    while(rc==0&&!socketp->isConnectComplete()&&!socketp->isRemoteClosed()&&!stop_requested)drive();
    if(socketp->isRemoteClosed()){TcpSocketMgr::freeSocket(socketp);socketp=0;return -1;}
    return 0;
}
int main(void) {
    int used,rc; uint16_t key;
    if(Utils::parseEnv()!=0)return 2;
    if(Utils::initStack(1,TCP_SOCKET_RING_SIZE,ctrl_break,ctrl_c))return 3;
    cprintf("[TCPAGENT] Connecting to 10.0.2.2:%u\r\n",SERVER_PORT);
    while(!stop_requested) {
        if(connect_host()!=0){unsigned long spins=0;while(spins++<60000UL&&!stop_requested)drive();continue;}
        cprintf("[TCPAGENT] Connected. Alt-X stops.\r\n"); write_text("TCPAGENT READY\r\n"); used=0;
        while(!stop_requested&&!socketp->isRemoteClosed()) {
            drive(); rc=socketp->recv((uint8_t *)data,CHUNK_SIZE);
            if(rc<0)break;
            for(int i=0;i<rc;++i) {
                int c=data[i];
                if(put_remaining) {
                    unsigned available=(unsigned)(rc-i);
                    unsigned take=put_remaining<available ? (unsigned)put_remaining : available;
                    if(fwrite(data+i,1,take,put_file)!=(size_t)take){fclose(put_file);put_file=0;put_remaining=0;error_text("Short write");}
                    else {
                        put_remaining-=take; i+=(int)take-1;
                        if(!put_remaining){fclose(put_file);put_file=0;ok_data((const unsigned char *)"",0);}
                    }
                } else if(c=='\r'||c=='\n') {
                    if(used){linebuf[used]='\0';process_line(linebuf);used=0;}
                } else if(used<LINE_SIZE-1) linebuf[used++]=(char)c;
            }
            if(_bios_keybrd(1)){key=_bios_keybrd(0);if((key&0xff)==3||(key>>8)==45)stop_requested=1;}
        }
        socketp->close(); TcpSocketMgr::freeSocket(socketp); socketp=0;
    }
    Utils::endStack(); return 0;
}
