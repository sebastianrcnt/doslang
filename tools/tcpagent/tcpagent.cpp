#include <bios.h>
#include <conio.h>
#include <dos.h>
#include <fcntl.h>
#include <io.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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
static unsigned long put_started;
static char put_path[260];

static unsigned long ticks(void) {
    long value=0;
    _bios_timeofday(_TIME_GETCLOCK,&value);
    return (unsigned long)value;
}
/* The BIOS tick is 18.2065 Hz, so one tick is 5.49254 hundredths of a second.
   549/100 keeps the error under 0.05% and cannot overflow 32 bits for a delta
   up to a full day (1573040 ticks * 549 fits). Plain delta/18 ran 1.1% fast. */
static void elapsed_text(unsigned long started,char *out) {
    unsigned long now=ticks(),delta=now>=started?now-started:now+(1573040UL-started);
    unsigned long hundredths=delta*549UL/100UL;
    sprintf(out,"%lu.%02lus",hundredths/100UL,hundredths%100UL);
}

/* Open Watcom's DOS conio has no textattr()/textcolor() -- it offers only
   cprintf/cputs/getch and friends -- and ANSI escapes are not interpreted on
   the installed FreeDOS console. So let cprintf lay the line out (it scrolls
   correctly) and then repaint the attribute bytes of the cells it just wrote.
   The cursor sits at column 0 of the row after the line, which is what lets us
   find those cells without tracking scrolling ourselves. */
#define TIMESTAMP_WIDTH 9
static void colorize(unsigned total,int attr) {
    unsigned char __far *vram; unsigned cols,used,row,col,start,k;
    if(*(unsigned char __far *)MK_FP(0x0040,0x0049)==7) return;  /* MDA: no color */
    cols=*(unsigned __far *)MK_FP(0x0040,0x004A);
    if(cols<40||cols>132) cols=80;
    used=(total+cols-1)/cols; if(!used) used=1;
    row=*(unsigned char __far *)MK_FP(0x0040,0x0051);
    if(row<used) return;            /* line scrolled off the top; nothing to paint */
    start=row-used;
    vram=(unsigned char __far *)MK_FP(0xB800,0);
    for(k=0;k<total;++k) {
        row=start+k/cols; col=k%cols;
        vram[((unsigned)row*cols+col)*2+1]=(unsigned char)(k<TIMESTAMP_WIDTH?0x08:attr);
    }
}
static void log_line(int attr,const char *fmt,...) {
    struct dostime_t now; va_list ap; char text[760];
    _dos_gettime(&now); va_start(ap,fmt); vsprintf(text,fmt,ap); va_end(ap);
    cprintf("%02u:%02u:%02u %s\r\n",now.hour,now.minute,now.second,text);
    colorize(TIMESTAMP_WIDTH+(unsigned)strlen(text),attr);
    { FILE *f=fopen("C:\\TCPAGENT.LOG","a");
      if(f){fprintf(f,"%02u:%02u:%02u %s\n",now.hour,now.minute,now.second,text);fclose(f);} }
}
static void init_log(void) {
    FILE *f=fopen("C:\\TCPAGENT.LOG","rb"); long size=0;
    if(f){fseek(f,0,SEEK_END);size=ftell(f);fclose(f);}
    if(size>262144L){remove("C:\\TCPAGENT.OLD");rename("C:\\TCPAGENT.LOG","C:\\TCPAGENT.OLD");}
    f=fopen("C:\\TCPAGENT.LOG","a");
    if(f){fputs("--- TCPAGENT start ---\n",f);fclose(f);}
}
/* PUT completes either here in command_put (zero length) or in the receive loop
   once the body arrives, so keep the one line both paths emit in one place. */
static void put_finished(void) {
    char elapsed[24]; elapsed_text(put_started,elapsed);
    log_line(0x0A,"< PUT %s OK %s",put_path,elapsed);
}

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
    if(!off){log_line(0x0C,"< READ ERR missing offset");error_text("READ requires path and offset");return;} *off++='\0';
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< READ ERR bad path encoding");error_text("Invalid path encoding");return;}
    pos=atol(off); log_line(0x0B,"> READ %s @%ld",path,pos);
    f=fopen(path,"rb"); if(!f){log_line(0x0C,"< READ ERR cannot open");error_text("Cannot open file");return;}
    if(fseek(f,pos,SEEK_SET)){fclose(f);log_line(0x0C,"< READ ERR cannot seek");error_text("Cannot seek file");return;}
    count=fread(data,1,CHUNK_SIZE,f); eof=count<CHUNK_SIZE; fclose(f);
    log_line(0x0A,"< READ %uB eof=%d",(unsigned)count,eof);
    sprintf(linebuf,"OK %d ",eof); write_text(linebuf); write_hex(data,count); write_text("\r\n");
}
static void command_write(char *args) {
    char path[260],*mode=strchr(args,' '),*payload; FILE *f; int count;
    if(!mode){log_line(0x0C,"< WRITE ERR missing mode");error_text("WRITE requires path, mode and data");return;} *mode++='\0';
    payload=strchr(mode,' '); if(!payload){log_line(0x0C,"< WRITE ERR missing data");error_text("WRITE requires data");return;} *payload++='\0';
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< WRITE ERR bad path encoding");error_text("Invalid path encoding");return;}
    count=decode_hex(payload,data,CHUNK_SIZE); if(count<0){log_line(0x0C,"< WRITE ERR bad data encoding");error_text("Invalid data encoding");return;}
    log_line(0x0B,"> WRITE %s %c %dB",path,mode[0]=='A'?'A':'T',count);
    f=fopen(path,mode[0]=='A'?"ab":"wb"); if(!f){log_line(0x0C,"< WRITE ERR cannot open");error_text("Cannot write file");return;}
    if(count&&fwrite(data,1,count,f)!=(size_t)count){fclose(f);log_line(0x0C,"< WRITE ERR short write");error_text("Short write");return;}
    fclose(f); log_line(0x0A,"< WRITE OK"); ok_data((const unsigned char *)"",0);
}
static void command_put(char *args) {
    char path[260],*length_text=strchr(args,' ');
    if(!length_text){log_line(0x0C,"< PUT ERR missing length");error_text("PUT requires path and length");return;}
    *length_text++='\0';
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< PUT ERR bad path encoding");error_text("Invalid path encoding");return;}
    put_remaining=strtoul(length_text,0,10); strcpy(put_path,path); put_started=ticks();
    log_line(0x0B,"> PUT %s %luB",put_path,put_remaining);
    put_file=fopen(path,"wb");
    if(!put_file){put_remaining=0;log_line(0x0C,"< PUT %s ERR cannot open",put_path);error_text("Cannot write file");return;}
    if(!put_remaining){fclose(put_file);put_file=0;put_finished();ok_data((const unsigned char *)"",0);}
}
static void command_get(char *args) {
    char path[260],elapsed[24]; FILE *f; long length; size_t count; unsigned long started=ticks();
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< GET ERR bad path encoding");error_text("Invalid path encoding");return;}
    log_line(0x0B,"> GET %s",path);
    f=fopen(path,"rb"); if(!f){log_line(0x0C,"< GET %s ERR cannot open",path);error_text("Cannot open file");return;}
    fseek(f,0,SEEK_END); length=ftell(f); fseek(f,0,SEEK_SET);
    sprintf(linebuf,"DATA %ld\r\n",length); write_text(linebuf);
    while((count=fread(data,1,CHUNK_SIZE,f))>0) if(send_all(data,count)<0)break;
    fclose(f); elapsed_text(started,elapsed);
    log_line(0x0A,"< GET OK %ldB %s",length,elapsed);
}
static void command_hash(char *args) {
    char path[260],elapsed[24]; FILE *f; size_t count; unsigned i;
    unsigned long length=0,hash=2166136261UL,started=ticks();
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< HASH ERR bad path encoding");error_text("Invalid path encoding");return;}
    log_line(0x0B,"> HASH %s",path);
    f=fopen(path,"rb"); if(!f){log_line(0x0C,"< HASH %s ERR cannot open",path);error_text("Cannot open file");return;}
    while((count=fread(data,1,CHUNK_SIZE,f))>0){length+=(unsigned long)count;for(i=0;i<count;++i){hash^=data[i];hash*=16777619UL;}}
    fclose(f); elapsed_text(started,elapsed);
    log_line(0x0A,"< HASH %luB %08lX %s",length,hash,elapsed);
    sprintf(linebuf,"STAT %lu %08lX\r\n",length,hash); write_text(linebuf);
}
static void command_list(char *args) {
    char path[260],pattern[300],output[CHUNK_SIZE]; struct find_t found;
    unsigned used=0,entries=0; int rc,truncated=0;
    if(!decode_path(args,path,sizeof(path))){log_line(0x0C,"< LIST ERR bad path encoding");error_text("Invalid path encoding");return;}
    log_line(0x0B,"> LIST %s",path);
    strcpy(pattern,path); if(pattern[0]&&pattern[strlen(pattern)-1]!='\\') strcat(pattern,"\\"); strcat(pattern,"*.*");
    rc=_dos_findfirst(pattern,_A_NORMAL|_A_RDONLY|_A_HIDDEN|_A_SYSTEM|_A_SUBDIR|_A_ARCH,&found);
    while(rc==0) { char entry[100]; int len;
        if(strcmp(found.name,".")&&strcmp(found.name,"..")) { sprintf(entry,"%s\t%lu\t%s\n",found.name,found.size,(found.attrib&_A_SUBDIR)?"DIR":"FILE"); len=strlen(entry); if(used+(unsigned)len>=sizeof(output)) {truncated=1;break;} memcpy(output+used,entry,len); used+=(unsigned)len; ++entries; }
        rc=_dos_findnext(&found);
    }
    log_line(truncated?0x0E:0x0A,"< LIST %u entries%s",entries,truncated?" (truncated)":"");
    ok_data((unsigned char *)output,used);
}
static void command_exec(char *args) {
    char command[700],elapsed[24]; const char *tmp="C:\\PIEXEC.TMP"; FILE *f;
    int n,code=-1,fd=-1,save1=-1,save2=-1; long length=0; size_t count; unsigned long started;
    n=decode_hex(args,(unsigned char *)command,sizeof(command)-1); if(n<0){error_text("Invalid command encoding");return;} command[n]='\0';
    started=ticks(); log_line(0x0E,"> EXEC %.640s",command);
    fflush(stdout); fflush(stderr);
    save1=dup(1); save2=dup(2);
    fd=open(tmp,O_CREAT|O_TRUNC|O_WRONLY|O_BINARY,S_IREAD|S_IWRITE);
    if(save1<0||save2<0||fd<0||dup2(fd,1)<0||dup2(fd,2)<0) {
        if(fd>=0)close(fd);
        if(save1>=0){dup2(save1,1);close(save1);}
        if(save2>=0){dup2(save2,2);close(save2);}
        log_line(0x0C,"< EXEC ERR redirect failed"); error_text("Cannot capture command output"); return;
    }
    close(fd); code=system(command); fflush(stdout); fflush(stderr);
    dup2(save1,1); dup2(save2,2); close(save1); close(save2);
    f=fopen(tmp,"rb");
    if(f){fseek(f,0,SEEK_END);length=ftell(f);fseek(f,0,SEEK_SET);}
    elapsed_text(started,elapsed);
    log_line(code?0x0C:0x0A,"< EXEC exit=%d %ldB %s",code,length,elapsed);
    sprintf(linebuf,"RESULT %d %ld 0\r\n",code,length); write_text(linebuf);
    while(f&&(count=fread(data,1,CHUNK_SIZE,f))>0)if(send_all(data,count)<0)break;
    if(f)fclose(f); remove(tmp);
}
static void process_line(char *line) {
    char *cmd=line,*args=strchr(line,' '); if(args)*args++='\0';else args=cmd+strlen(cmd);
    /* PING is deliberately not logged: wait-ready polls it twice a second. */
    if(!strcmp(cmd,"PING"))ok_data((const unsigned char *)"PONG",4);
    else if(!strcmp(cmd,"READ"))command_read(args);
    else if(!strcmp(cmd,"WRITE"))command_write(args);
    else if(!strcmp(cmd,"PUT"))command_put(args);
    else if(!strcmp(cmd,"GET"))command_get(args);
    else if(!strcmp(cmd,"HASH"))command_hash(args);
    else if(!strcmp(cmd,"LIST"))command_list(args);
    else if(!strcmp(cmd,"EXEC"))command_exec(args);
    else if(!strcmp(cmd,"QUIT")){log_line(0x07,"* QUIT received");ok_data((const unsigned char *)"BYE",3);stop_requested=1;}
    else { char message[96]; sprintf(message,"Unknown command: %.70s",cmd); log_line(0x0C,"< ERR %s",message); error_text(message); }
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
    int used,rc; uint16_t key; unsigned attempts=0;
    init_log();
    log_line(0x0B,"* TCPAGENT starting; Alt-X returns to DOS");
    if(Utils::parseEnv()!=0){log_line(0x0C,"* MTCP configuration error");return 2;}
    if(Utils::initStack(1,TCP_SOCKET_RING_SIZE,ctrl_break,ctrl_c)){log_line(0x0C,"* TCP stack initialization error");return 3;}
    while(!stop_requested) {
        if(!attempts)log_line(0x07,"* connecting 10.0.2.2:%u",SERVER_PORT);
        if(connect_host()!=0){unsigned long spins=0;++attempts;if(attempts==1||attempts%10==0)log_line(0x0C,"* connect failed; retry %u",attempts);while(spins++<60000UL&&!stop_requested)drive();continue;}
        attempts=0; log_line(0x0A,"* connected; host automation owns console");
        write_text("TCPAGENT READY\r\n"); used=0;
        while(!stop_requested&&!socketp->isRemoteClosed()) {
            drive(); rc=socketp->recv((uint8_t *)data,CHUNK_SIZE);
            if(rc<0)break;
            for(int i=0;i<rc;++i) {
                int c=data[i];
                if(put_remaining) {
                    unsigned available=(unsigned)(rc-i);
                    unsigned take=put_remaining<available ? (unsigned)put_remaining : available;
                    if(fwrite(data+i,1,take,put_file)!=(size_t)take){fclose(put_file);put_file=0;put_remaining=0;log_line(0x0C,"< PUT %s ERR short write (disk full?)",put_path);error_text("Short write");}
                    else {
                        put_remaining-=take; i+=(int)take-1;
                        if(!put_remaining){fclose(put_file);put_file=0;put_finished();ok_data((const unsigned char *)"",0);}
                    }
                } else if(c=='\r'||c=='\n') {
                    if(used){linebuf[used]='\0';process_line(linebuf);used=0;}
                } else if(used<LINE_SIZE-1) linebuf[used++]=(char)c;
            }
            if(_bios_keybrd(1)){key=_bios_keybrd(0);if((key&0xff)==3||(key>>8)==45)stop_requested=1;}
        }
        socketp->close(); TcpSocketMgr::freeSocket(socketp); socketp=0;
        if(!stop_requested)log_line(0x0C,"* link lost; retrying");
    }
    log_line(0x07,"* stopped; returning to DOS");
    Utils::endStack(); return 0;
}
