#include "emit_c.h"
#include <stdio.h>
#include <string.h>

static void pad(FeEmitter *e)
{
    int i;
    for (i = 0; i < e->indent; ++i) fputs("    ", e->out);
}

static const char *ctype(FeEmitter *e, FeNode *n)
{
    FeType *t;
    if (n && n->sem_type) t = n->sem_type;
    else if (n) t = fe_type_from_ast(&e->check->types, n);
    else t = fe_type_intern(&e->check->types, "i32");
    return fe_type_c_name(t, e->pointer_bits);
}

static const char *cname(FeNode *n, const char *fallback)
{
    return n && n->cname ? n->cname : fallback;
}

static void emit_one_type(FeEmitter *e, FeType *t);

static void emit_type_deps(FeEmitter *e, FeType *t)
{
    unsigned i,j;
    if (!t) return;
    if (t->kind == FE_TYPE_ARRAY) emit_one_type(e,t->elem);
    if (t->kind == FE_TYPE_STRUCT)
        for (i=0;i<t->field_count;i++) emit_one_type(e,t->fields[i].type);
    if (t->kind == FE_TYPE_ENUM)
        for (i=0;i<t->variant_count;i++)
            for (j=0;j<t->variants[i].field_count;j++)
                emit_one_type(e,t->variants[i].fields[j].type);
}

static void emit_one_type(FeEmitter *e, FeType *t)
{
    unsigned i,j;
    if (!t || t->emit_state ||
        (t->kind != FE_TYPE_STRUCT && t->kind != FE_TYPE_ENUM &&
         t->kind != FE_TYPE_ARRAY && t->kind != FE_TYPE_SLICE)) return;
    t->emit_state=1;
    emit_type_deps(e,t);
    if(t->kind==FE_TYPE_STRUCT) {
        fputs(t->cname,e->out); fputs(" {\n",e->out);
        for(i=0;i<t->field_count;i++) { fputs("    ",e->out); fputs(fe_type_c_name(t->fields[i].type,e->pointer_bits),e->out); fputc(' ',e->out); fputs(t->fields[i].name,e->out); fputs(";\n",e->out); }
        fputs("};\n",e->out);
    } else if(t->kind==FE_TYPE_ARRAY) {
        fputs(t->cname,e->out); fputs(" { ",e->out); fputs(fe_type_c_name(t->elem,e->pointer_bits),e->out); fputs(" a[",e->out); fprintf(e->out,"%lu",t->length); fputs("]; };\n",e->out);
    } else if(t->kind==FE_TYPE_SLICE && t->cname) {
        fputs("typedef struct { ",e->out); fputs(fe_type_c_name(t->elem,e->pointer_bits),e->out); fputs(" *p; unsigned long n; } ",e->out); fputs(t->cname,e->out); fputs(";\n",e->out);
        fprintf(e->out,"static %s %s(%s *p, unsigned long n) { %s s; s.p=p; s.n=n; return s; }\n",t->cname,t->maker,fe_type_c_name(t->elem,e->pointer_bits),t->cname);
    } else if(t->kind==FE_TYPE_ENUM) {
        for(i=0;i<t->variant_count;i++) if(t->variants[i].field_count>1) {
            fprintf(e->out,"struct fe_payload_%s_%s {",t->name,t->variants[i].name);
            for(j=0;j<t->variants[i].field_count;j++) { fputs(" ",e->out); fputs(fe_type_c_name(t->variants[i].fields[j].type,e->pointer_bits),e->out); fputc(' ',e->out); fputs(t->variants[i].fields[j].name,e->out); fputc(';',e->out); }
            fputs(" };\n",e->out);
        }
        fputs(t->cname,e->out); fputs(" { ",e->out); fputs(t->bits>8 ? "unsigned short" : "unsigned char",e->out); fputs(" tag; union { ",e->out);
        for(i=0;i<t->variant_count;i++) { if(t->variants[i].field_count==0) fputs("unsigned char",e->out); else if(t->variants[i].field_count==1) fputs(fe_type_c_name(t->variants[i].fields[0].type,e->pointer_bits),e->out); else fprintf(e->out,"struct fe_payload_%s_%s",t->name,t->variants[i].name); fputc(' ',e->out); fputs(t->variants[i].name,e->out); fputc(';',e->out); }
        fputs(" } payload; };\n",e->out);
    }
    t->emit_state=2;
}

static void emit_type_defs(FeEmitter *e)
{
    FeType *t;
    fputs("typedef struct { const unsigned char *p; unsigned long n; } fe_str;\n",e->out);
    fputs("static fe_str fe_make_str(const unsigned char *p, unsigned long n) { fe_str s; s.p=p; s.n=n; return s; }\n",e->out);
    /* Every fixed array has a slice conversion helper, even if this unit
       only indexes the array.  Intern those result types before emission so
       their typedefs are present before helper definitions. */
    for(t=e->check->types.types;t;t=t->next)
        if(t->kind==FE_TYPE_ARRAY) fe_type_slice(&e->check->types,t->elem);
    for(t=e->check->types.types;t;t=t->next) emit_one_type(e,t);
}

static void emit_type_helpers(FeEmitter *e)
{
    FeType *t;
    unsigned i,j;
    for(t=e->check->types.types;t;t=t->next) {
        if(t->kind==FE_TYPE_STRUCT && t->maker) {
            fprintf(e->out,"static %s %s(",t->cname,t->maker);
            for(i=0;i<t->field_count;i++) { if(i) fputs(", ",e->out); fputs(fe_type_c_name(t->fields[i].type,e->pointer_bits),e->out); fprintf(e->out," p%u",i); }
            fputs(") { ",e->out); fprintf(e->out,"%s v;",t->cname);
            for(i=0;i<t->field_count;i++) fprintf(e->out," v.%s=p%u;",t->fields[i].name,i);
            fputs(" return v; }\n",e->out);
        } else if(t->kind==FE_TYPE_ARRAY && t->maker) {
            fprintf(e->out,"static %s %s(",t->cname,t->maker);
            for(i=0;i<t->length;i++) { if(i) fputs(", ",e->out); fputs(fe_type_c_name(t->elem,e->pointer_bits),e->out); fprintf(e->out," p%u",i); }
            fputs(") { ",e->out); fprintf(e->out,"%s v;",t->cname);
            for(i=0;i<t->length;i++) fprintf(e->out," v.a[%u]=p%u;",i,i);
            fputs(" return v; }\n",e->out);
        } else if(t->kind==FE_TYPE_ENUM) {
            for(i=0;i<t->variant_count;i++) {
                FeVariantType *v=&t->variants[i];
                fprintf(e->out,"static %s %s(",t->cname,v->maker);
                for(j=0;j<v->field_count;j++) { if(j) fputs(", ",e->out); fputs(fe_type_c_name(v->fields[j].type,e->pointer_bits),e->out); fprintf(e->out," p%u",j); }
                fputs(") { ",e->out); fprintf(e->out,"%s x; x.tag=%u;",t->cname,v->tag);
                for(j=0;j<v->field_count;j++) { if(v->field_count==1) fprintf(e->out," x.payload.%s=p%u;",v->name,j); else fprintf(e->out," x.payload.%s.%s=p%u;",v->name,v->fields[j].name,j); }
                fputs(" return x; }\n",e->out);
            }
        }
    }
    for(t=e->check->types.types;t;t=t->next) {
        if (t->kind==FE_TYPE_ARRAY && t->indexer) {
            fprintf(e->out,"static %s %s(%s x, unsigned long i) { ",
                    fe_type_c_name(t->elem,e->pointer_bits),t->indexer,t->cname);
            if(!e->no_checks) fprintf(e->out,"if (i >= %lu) fe_trap_bounds(); ",t->length);
            fprintf(e->out,"return x.a[i]; }\n");
            fprintf(e->out,"static %s %s(%s x, unsigned long a, unsigned long b) { ",
                    fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->slicer,t->cname);
            if(!e->no_checks) fputs("if (a > b || b > ",e->out), fprintf(e->out,"%lu",t->length), fputs(") fe_trap_bounds(); ",e->out);
            fprintf(e->out,"return %s(x.a+a,b-a); }\n",fe_type_slice(&e->check->types,t->elem)->maker);
            fprintf(e->out,"static %s %s(%s x) { return %s(x,0,%lu); }\n",fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->full_slicer,t->cname,t->slicer,t->length);
            fprintf(e->out,"static %s %s(%s x, unsigned long a) { return %s(x,a,%lu); }\n",fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->tail_slicer,t->cname,t->slicer,t->length);
        } else if (t->kind==FE_TYPE_SLICE && t->indexer) {
            fprintf(e->out,"static %s %s(%s x, unsigned long i) { ",
                    fe_type_c_name(t->elem,e->pointer_bits),t->indexer,t->cname);
            if(!e->no_checks) fputs("if (i >= x.n) fe_trap_bounds(); ",e->out);
            fputs("return x.p[i]; }\n",e->out);
            fprintf(e->out,"static %s %s(%s x, unsigned long a, unsigned long b) { ",
                    fe_type_c_name(t,e->pointer_bits),t->slicer,t->cname);
            if(!e->no_checks) fputs("if (a > b || b > x.n) fe_trap_bounds(); ",e->out);
            fprintf(e->out,"return %s(x.p+a,b-a); }\n",t->maker);
            fprintf(e->out,"static %s %s(%s x) { return %s(x,0,x.n); }\n",t->cname,t->full_slicer,t->cname,t->slicer);
            fprintf(e->out,"static %s %s(%s x, unsigned long a) { return %s(x,a,x.n); }\n",t->cname,t->tail_slicer,t->cname,t->slicer);
        }
    }
    fputs("static unsigned char fe_idx_str(fe_str x, unsigned long i) { ",e->out);
    if(!e->no_checks) fputs("if (i >= x.n) fe_trap_bounds(); ",e->out);
    fputs("return x.p[i]; }\n",e->out);
    fputs("static fe_str fe_slice_str(fe_str x, unsigned long a, unsigned long b) { ",e->out);
    if(!e->no_checks) fputs("if (a > b || b > x.n) fe_trap_bounds(); ",e->out);
    fputs("return fe_make_str(x.p+a,b-a); }\n",e->out);
    fputs("static fe_str fe_full_slice_str(fe_str x) { return fe_slice_str(x,0,x.n); }\n",e->out);
    fputs("static fe_str fe_tail_slice_str(fe_str x, unsigned long a) { return fe_slice_str(x,a,x.n); }\n",e->out);
}

static void emit_expr(FeEmitter *e, FeNode *n);
static void emit_stmt(FeEmitter *e, FeNode *n);

static int stmt_definitely_returns(FeNode *n);

static int match_is_exhaustive(FeNode *n)
{
    FeType *t;
    FeNode *arm;
    unsigned i;
    int found;
    if (!n || !n->a) return 0;
    t=n->a->sem_type;
    if (!t || t->kind!=FE_TYPE_ENUM) return 0;
    for (arm=n->children; arm; arm=arm->next)
        if (arm->text && strcmp(arm->text,"_")==0) return 1;
    for (i=0; i<t->variant_count; ++i) {
        found=0;
        for (arm=n->children; arm; arm=arm->next)
            if (arm->text && strcmp(arm->text,t->variants[i].name)==0) {
                found=1;
                break;
            }
        if (!found) return 0;
    }
    return 1;
}

static int match_definitely_returns(FeNode *n)
{
    FeNode *arm;
    if (!match_is_exhaustive(n)) return 0;
    for (arm=n->children; arm; arm=arm->next)
        if (!stmt_definitely_returns(arm->a)) return 0;
    return 1;
}

static int stmt_definitely_returns(FeNode *n)
{
    FeNode *last;
    if (!n) return 0;
    if (n->kind==FE_N_RETURN) return 1;
    if (n->kind==FE_N_MATCH) return match_definitely_returns(n);
    if (n->kind==FE_N_BLOCK) {
        last=n->children;
        if (!last) return 0;
        while (last->next) last=last->next;
        return stmt_definitely_returns(last);
    }
    if (n->kind==FE_N_IF)
        return n->b && n->c && stmt_definitely_returns(n->b) &&
               stmt_definitely_returns(n->c);
    return 0;
}

static int hex_value(int c)
{
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}

static void emit_byte(FILE *out, unsigned value)
{
    fprintf(out,"\\%03o",value & 255U);
}

static void emit_codepoint(FILE *out, unsigned long cp)
{
    if (cp<=0x7fUL) emit_byte(out,(unsigned)cp);
    else if (cp<=0x7ffUL) {
        emit_byte(out,(unsigned)(0xc0UL | (cp>>6)));
        emit_byte(out,(unsigned)(0x80UL | (cp&0x3fUL)));
    } else if (cp<=0xffffUL) {
        emit_byte(out,(unsigned)(0xe0UL | (cp>>12)));
        emit_byte(out,(unsigned)(0x80UL | ((cp>>6)&0x3fUL)));
        emit_byte(out,(unsigned)(0x80UL | (cp&0x3fUL)));
    } else {
        emit_byte(out,(unsigned)(0xf0UL | (cp>>18)));
        emit_byte(out,(unsigned)(0x80UL | ((cp>>12)&0x3fUL)));
        emit_byte(out,(unsigned)(0x80UL | ((cp>>6)&0x3fUL)));
        emit_byte(out,(unsigned)(0x80UL | (cp&0x3fUL)));
    }
}

static void emit_c_literal(FILE *out, const char *text, int string)
{
    unsigned long i;
    unsigned long cp;
    int h0,h1,h2,h3;
    int c;
    char quote=string ? '"' : '\'';
    if (!text) { fputs(string ? "\"\"" : "'\\000'",out); return; }
    fputc(quote,out);
    for(i=1;text[i] && text[i]!=quote;i++) {
        c=(unsigned char)text[i];
        if(c!='\\') {
            if(c==quote || c=='\\') fputc('\\',out);
            fputc(c,out);
            continue;
        }
        ++i; c=(unsigned char)text[i];
        if(c=='u' && text[i+1] && text[i+2] && text[i+3] && text[i+4]) {
            h0=hex_value(text[i+1]); h1=hex_value(text[i+2]);
            h2=hex_value(text[i+3]); h3=hex_value(text[i+4]);
            if(h0>=0 && h1>=0 && h2>=0 && h3>=0) {
                cp=(unsigned long)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                emit_codepoint(out,cp); i+=4; continue;
            }
        }
        if(c=='x' && text[i+1] && text[i+2]) {
            h0=hex_value(text[i+1]); h1=hex_value(text[i+2]);
            if(h0>=0 && h1>=0) { emit_byte(out,(unsigned)((h0<<4)|h1)); i+=2; continue; }
        }
        if(c=='n') emit_byte(out,10U);
        else if(c=='r') emit_byte(out,13U);
        else if(c=='t') emit_byte(out,9U);
        else if(c=='0') emit_byte(out,0U);
        else emit_byte(out,(unsigned char)c);
    }
    fputc(quote,out);
}

static void emit_lvalue(FeEmitter *e, FeNode *n)
{
    FeType *bt;
    if (!n) { fputs("fe_bad_lvalue",e->out); return; }
    if (n->kind==FE_N_IDENT) { fputs(cname(n,"fe_local"),e->out); return; }
    if (n->kind==FE_N_MEMBER) {
        if (n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_REF &&
            n->b && n->b->text && strcmp(n->b->text,"^")==0) {
            fputs("(*",e->out); emit_expr(e,n->a); fputs(")",e->out);
        } else { emit_lvalue(e,n->a); fputc('.',e->out); fputs(n->b ? n->b->text : "member",e->out); }
        return;
    }
    if (n->kind==FE_N_INDEX) {
        bt=n->a ? n->a->sem_type : 0;
        emit_lvalue(e,n->a); fputs(bt && bt->kind==FE_TYPE_ARRAY ? ".a[" : ".p[",e->out);
        emit_expr(e,n->b); fputc(']',e->out); return;
    }
    emit_expr(e,n);
}

static FeNode *init_field(FeNode *n, const char *name)
{
    FeNode *f;
    for(f=n ? n->children : 0;f;f=f->next)
        if(f->kind==FE_N_FIELD && f->text && name && strcmp(f->text,name)==0) return f;
    return 0;
}

static void emit_slice_call(FeEmitter *e, FeNode *n)
{
    FeType *bt=n->a ? n->a->sem_type : 0;
    const char *maker=bt && bt->slicer ? bt->slicer : "fe_slice_str";
    if (!n->b && !n->c && bt && bt->full_slicer) {
        fputs(bt->full_slicer,e->out); fputc('(',e->out); emit_expr(e,n->a); fputc(')',e->out); return;
    }
    if (!n->c && bt && bt->tail_slicer) {
        fputs(bt->tail_slicer,e->out); fputc('(',e->out); emit_expr(e,n->a); fputs(", ",e->out);
        if (n->b) emit_expr(e,n->b); else fputs("0",e->out);
        fputc(')',e->out); return;
    }
    fputs(maker,e->out); fputc('(',e->out); emit_expr(e,n->a); fputs(", ",e->out);
    if(n->b) emit_expr(e,n->b); else fputs("0",e->out);
    fputs(", ",e->out);
    if(n->c) emit_expr(e,n->c);
    else if(bt && bt->kind==FE_TYPE_ARRAY) fprintf(e->out,"%lu",bt->length);
    else fputs("((unsigned long)",e->out), emit_expr(e,n->a), fputs(".n)",e->out);
    fputc(')',e->out);
}

static void emit_slice(FeEmitter *e, FeNode *n)
{
    emit_slice_call(e,n);
}

static void emit_expr(FeEmitter *e, FeNode *n)
{
    FeNode *x;
    const char *op;
    if (!n) {
        fputs("0", e->out);
        return;
    }
    switch (n->kind) {
    case FE_N_IDENT:
        fputs(cname(n, "fe_missing"), e->out);
        break;
    case FE_N_LITERAL:
        if (n->text && strcmp(n->text, "true") == 0) fputs("1", e->out);
        else if (n->text && strcmp(n->text, "false") == 0) fputs("0", e->out);
        else if (n->text && n->text[0]=='"') { fputs("fe_make_str((const unsigned char*)",e->out); emit_c_literal(e->out,n->text,1); fputs(", sizeof(",e->out); emit_c_literal(e->out,n->text,1); fputs(")-1)",e->out); }
        else if (n->text && n->text[0]=='\'') emit_c_literal(e->out,n->text,0);
        else fputs(n->text ? n->text : "0", e->out);
        break;
    case FE_N_STRUCT_INIT: {
        FeVariantType *v;
        FeNode *f;
        unsigned i;
        if(n->sem_type && n->a && n->a->kind==FE_N_MEMBER) {
            v=fe_type_variant(n->sem_type,n->a->b ? n->a->b->text : "");
            if(v) { fputs(v->maker,e->out); fputc('(',e->out); for(i=0;i<v->field_count;i++){f=init_field(n,v->fields[i].name);if(i)fputs(", ",e->out);if(f)emit_expr(e,f->a);else fputs("0",e->out);} fputc(')',e->out); }
            else fputs("0",e->out);
        } else if(n->sem_type && n->sem_type->maker) {
            fputs(n->sem_type->maker,e->out); fputc('(',e->out);
            if(n->sem_type->kind==FE_TYPE_STRUCT) { for(i=0;i<n->sem_type->field_count;i++){f=init_field(n,n->sem_type->fields[i].name);if(i)fputs(", ",e->out);if(f)emit_expr(e,f->a);else fputs("0",e->out);} }
            fputc(')',e->out);
        } else fputs("0",e->out);
        break;
    }
    case FE_N_ARRAY_INIT: {
        int first=1;
        if(n->sem_type && n->sem_type->maker) { fputs(n->sem_type->maker,e->out); fputc('(',e->out); for(x=n->children;x;x=x->next){if(!first)fputs(", ",e->out);emit_expr(e,x);first=0;} fputc(')',e->out); } else fputs("0",e->out);
        break;
    }
    case FE_N_INDEX: {
        FeType *bt;
        bt=n->a ? n->a->sem_type : 0;
        if(n->c || !n->b) {
            emit_slice(e,n);
        } else {
            if (bt && bt->indexer) {
                fputs(bt->indexer,e->out); fputc('(',e->out);
                emit_expr(e,n->a); fputs(", ",e->out); emit_expr(e,n->b); fputc(')',e->out);
            } else fputs("0",e->out);
        }
        break;
    }
    case FE_N_UNARY:
        op = n->text ? n->text : "";
        if (strcmp(op, "not") == 0) fputs("(!", e->out);
        else {
            fputc('(', e->out);
            fputs(op, e->out);
        }
        emit_expr(e, n->a);
        fputc(')', e->out);
        break;
    case FE_N_BINARY:
        op = n->text ? n->text : "+";
        fputc('(', e->out);
        emit_expr(e, n->a);
        if (strcmp(op, "and") == 0) fputs(" && ", e->out);
        else if (strcmp(op, "or") == 0) fputs(" || ", e->out);
        else fputs(op, e->out);
        emit_expr(e, n->b);
        fputc(')', e->out);
        break;
    case FE_N_TYPE:
        if (n->text && strcmp(n->text, "as") == 0) {
            fputs("((", e->out);
            fputs(ctype(e, n->b), e->out);
            fputc(')', e->out);
            emit_expr(e, n->a);
            fputc(')', e->out);
        } else emit_expr(e, n->a);
        break;
    case FE_N_CALL: {
        FeVariantType *v;
        int special=0;
        if(!n->a && n->text && strcmp(n->text,"@size_of")==0 && n->children && n->children->kind==FE_N_IDENT) { fprintf(e->out,"%lu",fe_type_size(fe_type_intern(&e->check->types,n->children->text))); special=1; }
        else if(!n->a && n->text && strcmp(n->text,"@align_of")==0 && n->children && n->children->kind==FE_N_IDENT) { fprintf(e->out,"%u",fe_type_align(fe_type_intern(&e->check->types,n->children->text))); special=1; }
        else if (n->a && n->a->kind==FE_N_MEMBER && n->a->a && n->a->a->sem_type && n->a->a->sem_type->kind==FE_TYPE_ENUM) {
            v=fe_type_variant(n->a->a->sem_type,n->a->b ? n->a->b->text : "");
            if(v) fputs(v->maker,e->out); else fputs("fe_bad_variant",e->out);
        } else if (n->a) emit_expr(e, n->a);
        else fputs(n->text ? n->text : "fe_builtin", e->out);
        if(!special) {
            fputc('(', e->out);
            for (x = n->children; x; x = x->next) {
                if (x != n->children) fputs(", ", e->out);
                emit_expr(e, x);
            }
            fputc(')', e->out);
        }
        break;
    }
    case FE_N_MEMBER: {
        FeVariantType *v;
        if(n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_REF &&
           n->b && n->b->text && strcmp(n->b->text,"^")==0) {
            fputs("(*",e->out); emit_expr(e,n->a); fputs(")",e->out);
        } else if(n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_ENUM) { v=fe_type_variant(n->a->sem_type,n->b ? n->b->text : ""); if(v) fputs(v->maker,e->out); else fputs("0",e->out); if(v)fputs("()",e->out); }
        else { emit_expr(e, n->a); fputc('.', e->out); if (n->b) fputs(n->b->text ? n->b->text : "member", e->out); }
        break;
    }
    default:
        fputs("0", e->out);
        break;
    }
}

static void emit_decl(FeEmitter *e, FeNode *n)
{
    pad(e);
    fputs(ctype(e, n), e->out);
    fputc(' ', e->out);
    fputs(cname(n, "fe_local"), e->out);
    fputs(";\n", e->out);
}

static void emit_block(FeEmitter *e, FeNode *n)
{
    FeNode *x;
    if (!n) {
        pad(e);
        fputs("{\n", e->out);
        ++e->indent;
        --e->indent;
        pad(e);
        fputc('}', e->out);
        return;
    }
    pad(e);
    fputs("{\n", e->out);
    ++e->indent;
    /* C89 requires declarations before statements in each actual block. */
    for (x = n->children; x; x = x->next)
        if (x->kind == FE_N_LET || x->kind == FE_N_VAR) emit_decl(e, x);
    for (x = n->children; x; x = x->next) emit_stmt(e, x);
    --e->indent;
    pad(e);
    fputc('}', e->out);
}

static void emit_match(FeEmitter *e, FeNode *n, int value_context)
{
    FeNode *arm;
    FeType *t=n->a ? n->a->sem_type : 0;
    FeVariantType *v;
    FeNode *b;
    unsigned i;
    char temp[32];
    sprintf(temp,"fe_match_%u",e->temp_serial++);
    pad(e); fputs("{\n",e->out); ++e->indent;
    pad(e); fputs(fe_type_c_name(t,e->pointer_bits),e->out); fputc(' ',e->out);
    fputs(temp,e->out); fputs(" = ",e->out); emit_expr(e,n->a); fputs(";\n",e->out);
    pad(e); fputs("switch (",e->out); fputs(temp,e->out); fputs(".tag) {\n",e->out); ++e->indent;
    for(arm=n->children;arm;arm=arm->next) {
        if(arm->text && strcmp(arm->text,"_")==0) { pad(e); fputs("default: ",e->out); }
        else { v=t && t->kind==FE_TYPE_ENUM ? fe_type_variant(t,arm->text) : 0; if(!v) continue; fprintf(e->out,"case %u: ",v->tag); }
        fputs("{\n",e->out); ++e->indent;
        v=t && t->kind==FE_TYPE_ENUM ? fe_type_variant(t,arm->text) : 0;
        if(v) for(i=0,b=arm->children;i<v->field_count && b;i++,b=b->next) {
            pad(e); fputs(fe_type_c_name(v->fields[i].type,e->pointer_bits),e->out); fputc(' ',e->out); fputs(cname(b,"fe_match"),e->out); fputs(" = ",e->out); fputs(temp,e->out); fputs(".payload.",e->out); fputs(v->name,e->out); if(v->field_count>1){fputc('.',e->out);fputs(v->fields[i].name,e->out);} fputs(";\n",e->out);
        }
        if(arm->a && arm->a->kind==FE_N_BLOCK) emit_stmt(e,arm->a); else { pad(e); emit_expr(e,arm->a); fputs(";\n",e->out); }
        pad(e); fputs("break;\n",e->out); --e->indent; pad(e); fputs("}\n",e->out);
    }
    --e->indent; pad(e); fputs("}\n",e->out);
    --e->indent; pad(e); fputs("}\n",e->out);
    if (value_context || match_definitely_returns(n)) {
        pad(e); fputs("fe_trap_bounds();\n",e->out);
        pad(e); fputs("return 0;\n",e->out);
    }
}

static void emit_stmt(FeEmitter *e, FeNode *n)
{
    if (!n) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        emit_block(e, n);
        fputc('\n', e->out);
        break;
    case FE_N_LET:
    case FE_N_VAR:
        if (n->b) {
            pad(e);
            fputs(cname(n, "fe_local"), e->out);
            fputs(" = ", e->out);
            emit_expr(e, n->b);
            fputs(";\n", e->out);
        }
        break;
    case FE_N_ASSIGN:
        pad(e);
        emit_lvalue(e, n->a);
        fputc(' ', e->out);
        fputs(n->text ? n->text : "=", e->out);
        fputs(" ", e->out);
        emit_expr(e, n->b);
        fputs(";\n", e->out);
        break;
    case FE_N_EXPR_STMT:
        pad(e);
        emit_expr(e, n->a);
        fputs(";\n", e->out);
        break;
    case FE_N_RETURN:
        pad(e);
        if (n->a && n->a->kind == FE_N_MATCH) {
            emit_match(e,n->a,1);
            break;
        }
        fputs("return", e->out);
        if (n->a) {
            fputc(' ', e->out);
            emit_expr(e, n->a);
        }
        fputs(";\n", e->out);
        break;
    case FE_N_IF:
        pad(e);
        fputs("if (", e->out);
        emit_expr(e, n->a);
        fputs(") ", e->out);
        if (n->b && n->b->kind == FE_N_BLOCK) emit_block(e, n->b);
        else emit_block(e, 0);
        if (n->c) {
            fputs(" else ", e->out);
            if (n->c->kind == FE_N_IF) emit_stmt(e, n->c);
            else emit_block(e, n->c);
        }
        fputc('\n', e->out);
        break;
    case FE_N_WHILE:
        pad(e);
        fputs("while (", e->out);
        emit_expr(e, n->a);
        fputs(") ", e->out);
        if (n->b && n->b->kind == FE_N_BLOCK) emit_block(e, n->b);
        else emit_block(e, 0);
        fputc('\n', e->out);
        break;
    case FE_N_FOR:
        pad(e); fputs("{\n",e->out); ++e->indent;
        if (n->c) {
            pad(e); fputs("unsigned long ",e->out); fputs(cname(n,"fe_index"),e->out); fputs(";\n",e->out);
            pad(e); fputs(cname(n,"fe_index"),e->out); fputs(" = ",e->out); emit_expr(e,n->a); fputs(";\n",e->out);
            pad(e); fputs("for (; ",e->out); fputs(cname(n,"fe_index"),e->out); fputs(" < ",e->out); emit_expr(e,n->c); fputs("; ++",e->out); fputs(cname(n,"fe_index"),e->out); fputs(") ",e->out); emit_block(e,n->b); fputc('\n',e->out);
        } else {
            FeType *bt=n->a ? n->a->sem_type : 0;
            FeType *et=bt ? bt->elem : 0;
            char temp[32];
            int mutable_iter=(n->flags & 4U) != 0;
            sprintf(temp,"fe_iter_%u",e->temp_serial++);
            pad(e); fputs(fe_type_c_name(bt,e->pointer_bits),e->out); if (mutable_iter) fputs(" *",e->out); fputc(' ',e->out); fputs(temp,e->out); fputs(" = ",e->out); if (mutable_iter) fputc('&',e->out); emit_expr(e,n->a); fputs(";\n",e->out);
            if (n->aux_text) {
                pad(e); fputs("unsigned long ",e->out); fputs(cname(n,"fe_index"),e->out); fputs(";\n",e->out);
            } else {
                pad(e); fputs(fe_type_c_name(n->sem_type ? n->sem_type : fe_type_ref(&e->check->types,et,0),e->pointer_bits),e->out); fputc(' ',e->out); fputs(cname(n,"fe_item"),e->out); fputs(";\n",e->out);
            }
            pad(e); fputs("{ unsigned long fe_i; for (fe_i = 0; fe_i < ",e->out);
            if(bt && bt->kind==FE_TYPE_ARRAY) fprintf(e->out,"%lu",bt->length); else { if(mutable_iter) fputs("(*",e->out); fputs(temp,e->out); if(mutable_iter) fputs(").n",e->out); else fputs(".n",e->out); }
            fputs("; ++fe_i) { ",e->out);
            if(n->aux_text) {
                fputs(fe_type_c_name(fe_type_ref(&e->check->types,et,(n->flags & 4U) != 0),e->pointer_bits),e->out); fputc(' ',e->out); fputs(n->aux_cname ? n->aux_cname : "fe_item",e->out); fputs("; ",e->out);
                fputs(cname(n,"fe_index"),e->out); fputs(" = fe_i; ",e->out);
                fputs(n->aux_cname ? n->aux_cname : "fe_item",e->out); fputs(" = ",e->out);
            } else { fputs(cname(n,"fe_item"),e->out); fputs(" = ",e->out); }
            fputc('&',e->out); if(mutable_iter) fputs("(*",e->out); fputs(temp,e->out); if(mutable_iter) fputs(")",e->out); if(bt && bt->kind==FE_TYPE_ARRAY) fputs(".a[fe_i]",e->out); else fputs(".p[fe_i]",e->out); fputs("; ",e->out);
            emit_block(e,n->b); fputs(" } }\n",e->out);
        }
        --e->indent; pad(e); fputs("}\n",e->out); break;
    case FE_N_MATCH:
        emit_match(e,n,0); break;
    default:
        break;
    }
}

static void emit_fn(FeEmitter *e, FeNode *fn, int prototype)
{
    FeNode *p;
    const char *ret;
    ret = fn->sem_type ? fe_type_c_name(fn->sem_type, e->pointer_bits) :
        (fn->b ? ctype(e, fn->b) : "void");
    fputs(ret, e->out);
    fputc(' ', e->out);
    fputs(cname(fn, "fe_fn"), e->out);
    fputc('(', e->out);
    p = fn->a ? fn->a->children : 0;
    if (!p) fputs("void", e->out);
    while (p) {
        if (p != fn->a->children) fputs(", ", e->out);
        fputs(ctype(e, p->a), e->out);
        fputc(' ', e->out);
        fputs(cname(p, "fe_arg"), e->out);
        p = p->next;
    }
    fputc(')', e->out);
    if (prototype) fputs(";\n", e->out);
    else {
        fputs(" ", e->out);
        emit_block(e, fn->c);
        fputc('\n', e->out);
    }
}

static void emit_main_wrapper(FeEmitter *e, FeNode *fn)
{
    fputs("int main(void) {\n    ", e->out);
    if (fn->sem_type && fn->sem_type->kind == FE_TYPE_VOID) {
        fputs(cname(fn, "fe_main"), e->out);
        fputs("();\n    return 0;\n", e->out);
    } else {
        fputs("return ", e->out);
        fputs(cname(fn, "fe_main"), e->out);
        fputs("();\n", e->out);
    }
    fputs("}\n", e->out);
}

void fe_emit_c_init(FeEmitter *e, FILE *out, FeCheck *check,
                    unsigned pointer_bits, int no_checks)
{
    e->out = out;
    e->check = check;
    e->pointer_bits = pointer_bits;
    e->indent = 0;
    e->no_checks = no_checks;
    e->temp_serial = 0;
}

void fe_emit_c_program(FeEmitter *e)
{
    FeNode *n;
    FeNode *main_fn = 0;
    fputs("/* generated by fec M3 */\n#include <stddef.h>\n#include <stdlib.h>\ntypedef char fe_assert_u8[(sizeof(unsigned char)==1) ? 1 : -1];\ntypedef char fe_assert_u16[(sizeof(unsigned short)==2) ? 1 : -1];\ntypedef char fe_assert_u32[(sizeof(unsigned long)==4) ? 1 : -1];\n", e->out);
    if (e->pointer_bits==16)
        fputs("typedef char fe_assert_usize[(sizeof(unsigned short)==2) ? 1 : -1];\n",e->out);
    else
        fputs("typedef char fe_assert_usize[(sizeof(unsigned long)==4) ? 1 : -1];\n",e->out);
    fputs("static void fe_trap_bounds(void) { abort(); }\n\n", e->out);
    emit_type_defs(e);
    emit_type_helpers(e);
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            fputs(ctype(e, n), e->out);
            fputc(' ', e->out);
            fputs(cname(n, "fe_global"), e->out);
            if (n->b) {
                fputs(" = ", e->out);
                emit_expr(e, n->b);
            }
            fputs(";\n", e->out);
        }
    }
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next)
        if (n->kind == FE_N_FN) {
            emit_fn(e, n, 1);
            if (n->text && strcmp(n->text, "main") == 0) main_fn = n;
        }
    fputc('\n', e->out);
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next)
        if (n->kind == FE_N_FN) emit_fn(e, n, 0);
    if (main_fn) {
        fputc('\n', e->out);
        emit_main_wrapper(e, main_fn);
    }
}
