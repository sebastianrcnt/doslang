#include "diag.h"

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
    fputs("  ",stderr);
    fprintf(stderr,"%lu | ",loc.line);
    if(end>start) fwrite(src+start,1,(size_t)(end-start),stderr);
    fputc('\n',stderr);
    fputs("  ",stderr);
    for(i=0;i<gutter;++i) fputc(' ',stderr);
    fputs(" | ",stderr);
    col=1;
    i=start;
    while(i<end && col<loc.col){
        c=src[i++];
        fputc(c=='\t' ? '\t' : ' ',stderr);
        ++col;
    }
    fputs("^\n",stderr);
}

void fe_diags_init(FeDiags *d, const char *source, unsigned long source_len)
{
    d->errors=0;
    d->warnings=0;
    d->source=source;
    d->source_len=source_len;
}

void fe_diag_error(FeDiags *d, FeLoc loc, const char *msg)
{
    d->errors++;
    fprintf(stderr, "%s:%lu:%lu: error: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
    excerpt(d,loc);
}

void fe_diag_errorf(FeDiags *d, FeLoc loc, const char *msg, const char *arg)
{
    d->errors++;
    fprintf(stderr, "%s:%lu:%lu: error: ", loc.file ? loc.file : "<source>", loc.line, loc.col);
    fprintf(stderr, msg, arg);
    fputc('\n', stderr);
    excerpt(d,loc);
}

void fe_diag_note(FeLoc loc, const char *msg)
{
    fprintf(stderr, "%s:%lu:%lu: note: %s\n", loc.file ? loc.file : "<source>", loc.line, loc.col, msg);
}

void fe_diag_note_src(FeDiags *d, FeLoc loc, const char *msg)
{
    fe_diag_note(loc,msg);
    excerpt(d,loc);
}
