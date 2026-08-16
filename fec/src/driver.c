#include "parser.h"
#include "check.h"
#include "resolve.h"
#include "lower.h"
#include "x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *name, unsigned long *size)
{
    FILE *f; long n; char *p;
    f=fopen(name,"rb"); if(!f){fprintf(fe_diag_stream(),"fec: cannot open %s\n",name);return 0;}
    if(fseek(f,0L,SEEK_END)!=0){fclose(f);return 0;} n=ftell(f); if(n<0){fclose(f);return 0;} rewind(f);
    p=(char *)malloc((unsigned long)n+1); if(!p){fclose(f);return 0;}
    if(n && fread(p,1,(size_t)n,f)!=(size_t)n){free(p);fclose(f);return 0;} fclose(f);p[n]='\0';*size=(unsigned long)n;return p;
}

static void usage(void)
{
    puts("usage: fec [--dump-tokens|--dump-ast|--check|--dump-ir|--emit-asm] file.fe [-o out.asm] [--no-checks]");
}

static void dump_tokens(const char *src, unsigned long n, const char *file,
                        FeDiags *d)
{
    FeLexer lexer;
    FeToken tok;
    fe_lexer_init(&lexer,src,n,file,d);
    do {
        tok=fe_lexer_next(&lexer);
        fprintf(stdout,"%lu:%lu\t%s\t",tok.loc.line,tok.loc.col,
                fe_token_name(tok.kind));
        if(tok.length) fwrite(tok.begin,1,(size_t)tok.length,stdout);
        else fputc('-',stdout);
        fputc('\n',stdout);
    } while(tok.kind!=FE_TOK_EOF);
}

int main(int argc, char **argv)
{
    int i,dump=0,dump_tok=0,check_only=0,no_checks=0,dump_ir=0,emit_asm=0;
    const char *file=0;
    const char *out_path=0;
    unsigned long n;
    char *src;
    FeDiags d;
    FeAst ast;
    FeParser p;
    FeCheck check;
    unsigned pointer_bits=FE_PTR_BITS;
    if(argc<2){usage();return 2;}
    for(i=1;i<argc;i++) {
        if(strcmp(argv[i],"--dump-ast")==0) dump=1;
        else if(strcmp(argv[i],"--dump-tokens")==0) dump_tok=1;
        else if(strcmp(argv[i],"--check")==0) check_only=1;
        else if(strcmp(argv[i],"--dump-ir")==0) dump_ir=1;
        else if(strcmp(argv[i],"--emit-asm")==0) emit_asm=1;
        else if(strcmp(argv[i],"-o")==0 && i+1<argc) out_path=argv[++i];
        else if(strcmp(argv[i],"--no-checks")==0) no_checks=1;
        else if(strncmp(argv[i],"--target=",9)==0 || strncmp(argv[i],"--model=",8)==0 || strcmp(argv[i],"--strip-error-names")==0) { }
        else if(argv[i][0]!='-') file=argv[i];
        else if(strcmp(argv[i],"--help")==0){usage();return 0;}
        else {fprintf(fe_diag_stream(),"fec: unknown option %s\n",argv[i]);return 2;}
    }
    if((dump?1:0)+(dump_tok?1:0)+(check_only?1:0)+(dump_ir?1:0)+(emit_asm?1:0)>1){
        fprintf(fe_diag_stream(),"fec: choose only one output mode\n");
        return 2;
    }
    if(!file){fprintf(fe_diag_stream(),"fec: no input file\n");return 2;}
    src=read_file(file,&n);
    if(!src)return 2;
    fe_diags_init(&d,src,n);
    if(dump_tok){
        dump_tokens(src,n,file,&d);
        free(src);
        return d.errors?1:0;
    }
    fe_ast_init(&ast);
    fe_parser_init(&p,&ast,src,n,file,&d);
    ast.root=fe_parse_unit(&p);
    if(dump){
        fe_ast_dump(ast.root,0,stdout);
        fe_ast_destroy(&ast);
        free(src);
        return d.errors?1:0;
    }
    fe_ast_destroy(&ast);
    free(src);
    src=0;
    /* Load the whole unit graph rooted at this file: identity, imports,
       cycles and bindings. The entry file is parsed a second time as part of
       it, which costs one file read and keeps the graph the single owner of
       every unit's AST. */
    {
        FeBuild build;
        int ok=fe_build_load(&build,file,&d);
        if(ok){
            fe_check_init(&check,&build,&d,pointer_bits,no_checks);
            if(!fe_check_program(&check)) ok=0;
            if(ok && (dump_ir||emit_asm)){
                FeIrModule ir;
                fe_ir_module_init(&ir);
                if(!fe_lower_program(&check,&ir)) ok=0;
                else if(dump_ir) fe_ir_dump(&ir,stdout);
                else {
                    FILE *o=out_path?fopen(out_path,"w"):stdout;
                    if(!o){fprintf(fe_diag_stream(),"fec: cannot write %s\n",out_path);ok=0;}
                    else { fe_x86_emit(&ir,o); if(out_path) fclose(o); }
                }
                fe_ir_module_destroy(&ir);
            }
            fe_check_destroy(&check);
        }
        fe_build_destroy(&build);
        /* Semantic analysis is the last pass there is. A code generator
           attaches here; until then --check and the default path agree. */
        (void)check_only;
        return (!ok||d.errors)?1:0;
    }
}
