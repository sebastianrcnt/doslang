#include "diag.h"

#include <stdlib.h>

FILE *fe_diag_stream(void)
{
    static FILE *stream=0;
    if(!stream) stream=getenv("FE_DIAG_STDOUT") ? stdout : stderr;
    return stream;
}

static unsigned long digits(unsigned long n)
{
    unsigned long count=1;
    while(n>=10UL){n/=10UL;++count;}
    return count;
}

static void excerpt(const FeDiags *d, FeLoc loc)
{
    const char *src;
    unsigned long len;
    unsigned long i=0;
    unsigned long line=1;
    unsigned long start;
    unsigned long end;
    unsigned long gutter;
    unsigned long col;
    char c;
    if(!d || !d->source || !d->source_len || !loc.line || !loc.col) return;
    src=d->source;
    len=d->source_len;
    while(i<len && line<loc.line){if(src[i++]=='\n')++line;}
    if(line!=loc.line || i>len) return;
    start=i;
    end=start;
    while(end<len && src[end]!='\n' && src[end]!='\r')++end;
    gutter=digits(loc.line);
    fputs("  ",fe_diag_stream());
    fprintf(fe_diag_stream(),"%lu | ",loc.line);
    if(end>start) fwrite(src+start,1,(size_t)(end-start),fe_diag_stream());
    fputc('\n',fe_diag_stream());
    fputs("  ",fe_diag_stream());
    for(i=0;i<gutter;++i) fputc(' ',fe_diag_stream());
    fputs(" | ",fe_diag_stream());
    col=1;
    i=start;
    while(i<end && col<loc.col){
        c=src[i++];
        fputc(c=='\t' ? '\t' : ' ',fe_diag_stream());
        ++col;
    }
    fputs("^\n",fe_diag_stream());
}

void fe_diags_init(FeDiags *d, const char *source, unsigned long source_len)
{
    d->errors=0;
    d->warnings=0;
    d->source=source;
    d->source_len=source_len;
}

void fe_diags_source(FeDiags *d, const char *source, unsigned long source_len)
{
    d->source=source;
    d->source_len=source_len;
}

void fe_diag_error(FeDiags *d, FeLoc loc, const char *msg)
{
    d->errors++;
    fprintf(fe_diag_stream(), "%s:%lu:%lu: error: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
    excerpt(d,loc);
}

void fe_diag_errorf(FeDiags *d, FeLoc loc, const char *msg, const char *arg)
{
    d->errors++;
    fprintf(fe_diag_stream(), "%s:%lu:%lu: error: ", loc.file ? loc.file : "<source>", loc.line, loc.col);
    fprintf(fe_diag_stream(), msg, arg);
    fputc('\n', fe_diag_stream());
    excerpt(d,loc);
}

void fe_diag_note(FeLoc loc, const char *msg)
{
    fprintf(fe_diag_stream(), "%s:%lu:%lu: note: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
}

void fe_diag_note_src(FeDiags *d, FeLoc loc, const char *msg)
{
    fe_diag_note(loc,msg);
    excerpt(d,loc);
}
