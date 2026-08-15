#include "parser.h"
#include "check.h"
#include "emit_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *name, unsigned long *size)
{
    FILE *f; long n; char *p;
    f=fopen(name,"rb"); if(!f){fprintf(stderr,"fec: cannot open %s\n",name);return 0;}
    if(fseek(f,0L,SEEK_END)!=0){fclose(f);return 0;} n=ftell(f); if(n<0){fclose(f);return 0;} rewind(f);
    p=(char *)malloc((unsigned long)n+1); if(!p){fclose(f);return 0;}
    if(n && fread(p,1,(size_t)n,f)!=(size_t)n){free(p);fclose(f);return 0;} fclose(f);p[n]='\0';*size=(unsigned long)n;return p;
}
static void usage(void)
{ puts("usage: fec [--dump-ast|--emit-c] file.fe [--target=bits16|bits32] [-o output.c]"); }
int main(int argc, char **argv)
{
    int i,dump=0,emit=0; const char *file=0,*outname=0; unsigned long n; char *src; FeDiags d; FeAst ast; FeParser p; FeCheck check; FeEmitter emitter; FILE *out; unsigned pointer_bits=32;
    (void)emit;
    if(argc<2){usage();return 2;}
    for(i=1;i<argc;i++) { if(strcmp(argv[i],"--dump-ast")==0) dump=1; else if(strcmp(argv[i],"--emit-c")==0) emit=1; else if(strcmp(argv[i],"-o")==0){if(i+1>=argc){fprintf(stderr,"fec: -o needs a path\n");return 2;}outname=argv[++i];} else if(strncmp(argv[i],"-o",2)==0 && argv[i][2]) outname=argv[i]+2; else if(strncmp(argv[i],"--target=bits16",15)==0) pointer_bits=16; else if(strncmp(argv[i],"--target=bits32",15)==0) pointer_bits=32; else if(strncmp(argv[i],"--target=",9)==0 || strncmp(argv[i],"--model=",8)==0 || strcmp(argv[i],"--no-checks")==0 || strcmp(argv[i],"--strip-error-names")==0) { } else if(argv[i][0]!='-') file=argv[i]; else if(strcmp(argv[i],"--help")==0){usage();return 0;} else {fprintf(stderr,"fec: unknown option %s\n",argv[i]);return 2;} }
    if(!file){fprintf(stderr,"fec: no input file\n");return 2;}
    src=read_file(file,&n);if(!src)return 2;d.errors=0;d.warnings=0;fe_ast_init(&ast);fe_parser_init(&p,&ast,src,n,file,&d);ast.root=fe_parse_unit(&p);
    if(dump) { fe_ast_dump(ast.root,0,stdout); fe_ast_destroy(&ast); free(src); return d.errors?1:0; }
    fe_check_init(&check,&ast,&d,pointer_bits); if(!fe_check_program(&check)){fe_ast_destroy(&ast);free(src);return 1;}
    out=outname?fopen(outname,"w"):stdout; if(!out){fprintf(stderr,"fec: cannot create %s\n",outname);fe_ast_destroy(&ast);free(src);return 2;}
    fe_emit_c_init(&emitter,out,&check,pointer_bits);fe_emit_c_program(&emitter);if(outname)fclose(out);
    fe_ast_destroy(&ast); free(src); return d.errors?1:0;
}
