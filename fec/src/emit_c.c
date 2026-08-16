#include "emit_c.h"
#include <stdio.h>
#include <string.h>

static void emit_expr(FeEmitter *e, FeNode *n);
static void emit_stmt(FeEmitter *e, FeNode *n);
static void emit_block(FeEmitter *e, FeNode *n);

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

static int type_needs_drop(FeType *t)
{
    unsigned i;
    if (!t) return 0;
    if (t->kind==FE_TYPE_OWNED) return 1;
    if (t->kind==FE_TYPE_ARRAY) return type_needs_drop(t->elem);
    if (t->kind==FE_TYPE_STRUCT) {
        if (t->has_drop) return 1;
        for (i=0;i<t->field_count;i++) if (type_needs_drop(t->fields[i].type)) return 1;
    }
    return 0;
}

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
    if (t->kind == FE_TYPE_ERROR_UNION && t->error_value)
        emit_one_type(e,t->error_value);
}

static void emit_one_type(FeEmitter *e, FeType *t)
{
    unsigned i,j;
    if (t && strcmp(t->name,"io.Writer")==0) return;
    if (!t || t->emit_state ||
        (t->kind != FE_TYPE_STRUCT && t->kind != FE_TYPE_ENUM &&
         t->kind != FE_TYPE_ARRAY && t->kind != FE_TYPE_SLICE &&
         t->kind != FE_TYPE_ERROR_UNION) ||
        (t->kind == FE_TYPE_ERROR_UNION &&
         (!t->error_value || t->error_value->kind == FE_TYPE_VOID))) return;
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
    } else if(t->kind==FE_TYPE_ERROR_UNION) {
        fputs(t->cname,e->out); fputs(" { unsigned short e; ",e->out);
        fputs(fe_type_c_name(t->error_value,e->pointer_bits),e->out);
        fputs(" v; } ;\n",e->out);
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

static FeNode *find_drop_method(FeEmitter *e, const char *name)
{
    FeNode *n;
    FeNode *m;
    for (n=e->check->ast->root ? e->check->ast->root->children : 0; n; n=n->next)
        if (n->kind==FE_N_STRUCT && n->text && name && strcmp(n->text,name)==0)
            for (m=n->children; m; m=m->next)
                if (m->kind==FE_N_FN && m->text && strcmp(m->text,"drop")==0)
                    return m;
    return 0;
}

static void emit_drop_fields(FeEmitter *e, FeType *t)
{
    unsigned i;
    FeType *ft;
    for (i=t->field_count; i>0; --i) {
        ft=t->fields[i-1].type;
        if (!type_needs_drop(ft)) continue;
        if (ft->kind==FE_TYPE_OWNED) {
            fputs("if (self->",e->out); fputs(t->fields[i-1].name,e->out);
            fputs(") { ",e->out);
            if (ft->elem && type_needs_drop(ft->elem) && ft->elem->drop_cname) {
                fprintf(e->out,"%s(self->%s); ",ft->elem->drop_cname,t->fields[i-1].name);
            }
            fputs("free(self->",e->out); fputs(t->fields[i-1].name,e->out);
            fputs("); self->",e->out); fputs(t->fields[i-1].name,e->out);
            fputs("=0; }\n",e->out);
        } else if (ft->kind==FE_TYPE_STRUCT && ft->drop_cname) {
            fprintf(e->out,"%s(&self->%s);\n",ft->drop_cname,t->fields[i-1].name);
        } else if (ft->kind==FE_TYPE_ARRAY && ft->drop_cname) {
            fprintf(e->out,"%s(&self->%s);\n",ft->drop_cname,t->fields[i-1].name);
        }
    }
}

static void emit_drop_helpers(FeEmitter *e)
{
    FeType *t;
    FeNode *method;
    FeNode *param;
    for (t=e->check->types.types; t; t=t->next)
        if ((t->kind==FE_TYPE_STRUCT || t->kind==FE_TYPE_ARRAY) &&
            type_needs_drop(t) && t->drop_cname)
            fprintf(e->out,"static void %s(%s *self);\n",t->drop_cname,t->cname);
    for (t=e->check->types.types; t; t=t->next) {
        if (t->kind!=FE_TYPE_STRUCT || !type_needs_drop(t) || !t->drop_cname) continue;
        fprintf(e->out,"static void %s(%s *self) {\n",t->drop_cname,t->cname);
        method=find_drop_method(e,t->name);
        if (method) {
            param=method->a ? method->a->children : 0;
            if (param) param->cname="self";
            if (method->c) emit_block(e,method->c);
            fputc('\n',e->out);
        }
        emit_drop_fields(e,t);
        fputs("}\n",e->out);
    }
    for (t=e->check->types.types; t; t=t->next)
        if (t->kind==FE_TYPE_ARRAY && type_needs_drop(t) && t->drop_cname) {
            fprintf(e->out,"static void %s(%s *self) { unsigned long i; for (i=0; i<%lu; ++i) { ",
                    t->drop_cname,t->cname,t->length);
            if (t->elem->kind==FE_TYPE_OWNED) {
                fputs("if (self->a[i]) { ",e->out);
                if (t->elem->elem && type_needs_drop(t->elem->elem) && t->elem->elem->drop_cname)
                    fprintf(e->out,"%s(self->a[i]); ",t->elem->elem->drop_cname);
                fputs("free(self->a[i]); self->a[i]=0; }",e->out);
            } else if (t->elem->drop_cname)
                fprintf(e->out,"%s(&self->a[i]);",t->elem->drop_cname);
            fputs(" } }\n",e->out);
        }
}

static void emit_type_helpers(FeEmitter *e)
{
    FeType *t;
    unsigned i,j;
    for(t=e->check->types.types;t;t=t->next) {
        if (t->kind==FE_TYPE_ERROR_UNION && t->error_value &&
            t->error_value->kind!=FE_TYPE_VOID) {
            fprintf(e->out,"static %s %s(unsigned short e, %s v) { %s r; r.e=e; r.v=v; return r; }\n",
                    t->cname,t->maker,fe_type_c_name(t->error_value,e->pointer_bits),t->cname);
            if (t->error_value->kind==FE_TYPE_OWNED)
                fprintf(e->out,"static %s %s(void) { %s r; r.v=(%s)malloc(sizeof(%s)); r.e=r.v ? 0 : 1; return r; }\n",
                        t->cname,t->alloc_cname,t->cname,
                        fe_type_c_name(t->error_value,e->pointer_bits),
                        fe_type_c_name(t->error_value->elem,e->pointer_bits));
        }
    }
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
    emit_drop_helpers(e);
    for(t=e->check->types.types;t;t=t->next) {
        if (t->kind==FE_TYPE_ARRAY && t->indexer) {
            fprintf(e->out,"static %s %s(%s x, unsigned long i) { ",
                    fe_type_c_name(t->elem,e->pointer_bits),t->indexer,t->cname);
            if(!e->no_checks) fprintf(e->out,"if (i >= %lu) fe_trap_bounds(); ",t->length);
            fprintf(e->out,"return x.a[i]; }\n");
            fprintf(e->out,"static %s %s(%s *x, unsigned long a, unsigned long b) { ",
                    fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->slicer,t->cname);
            if(!e->no_checks) fputs("if (a > b || b > ",e->out), fprintf(e->out,"%lu",t->length), fputs(") fe_trap_bounds(); ",e->out);
            fprintf(e->out,"return %s(x->a+a,b-a); }\n",fe_type_slice(&e->check->types,t->elem)->maker);
            fprintf(e->out,"static %s %s(%s *x) { return %s(x,0,%lu); }\n",fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->full_slicer,t->cname,t->slicer,t->length);
            fprintf(e->out,"static %s %s(%s *x, unsigned long a) { return %s(x,a,%lu); }\n",fe_type_c_name(fe_type_slice(&e->check->types,t->elem),e->pointer_bits),t->tail_slicer,t->cname,t->slicer,t->length);
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

static void emit_m4_runtime(FeEmitter *e)
{
    fputs("typedef struct { unsigned char *p; unsigned long n; } fe_m4_slice;\n",e->out);
    fputs("typedef struct { void *ctx; unsigned short (*write_fn)(void *, const unsigned char *, unsigned long); } fe_writer;\n",e->out);
    fputs("unsigned short fe_m4_error;\n",e->out);
    fputs("unsigned short fe_m4_stdout_write(void *ctx, const unsigned char *p, unsigned long n) { (void)ctx; return fwrite(p,1,(size_t)n,stdout)==(size_t)n ? 0 : 1; }\n",e->out);
    fputs("unsigned short fe_m4_null_write(void *ctx, const unsigned char *p, unsigned long n) { (void)ctx; (void)p; (void)n; return 0; }\n",e->out);
    fputs("unsigned short fe_m4_buf_write(void *ctx, const unsigned char *p, unsigned long n) { fe_m4_slice *b=(fe_m4_slice*)ctx; unsigned long k=n<b->n?n:b->n; if(k) memcpy(b->p,p,(size_t)k); b->p+=k; b->n-=k; return 0; }\n",e->out);
    fputs("fe_writer fe_m4_stdout_writer(void) { fe_writer w; w.ctx=0; w.write_fn=fe_m4_stdout_write; return w; }\n",e->out);
    fputs("fe_writer fe_m4_null_writer(void) { fe_writer w; w.ctx=0; w.write_fn=fe_m4_null_write; return w; }\n",e->out);
    fputs("fe_writer fe_m4_buf_writer(fe_m4_slice *b) { fe_writer w; w.ctx=b; w.write_fn=fe_m4_buf_write; return w; }\n",e->out);
    fputs("/* bounded sprint stack; overflow traps instead of corrupting an outer call */\n#define FE_M4_SPRINT_DEPTH 8\n",e->out);
    fputs("typedef struct { fe_m4_slice b; unsigned long start_n; } fe_m4_sprint_frame;\n",e->out);
    fputs("static fe_m4_sprint_frame fe_m4_sprint_stack[FE_M4_SPRINT_DEPTH];\n",e->out);
    fputs("static unsigned fe_m4_sprint_depth;\n",e->out);
    fputs("void fe_m4_sprint_begin(fe_m4_slice *b) { if (fe_m4_sprint_depth>=FE_M4_SPRINT_DEPTH) abort(); fe_m4_sprint_stack[fe_m4_sprint_depth].b=*b; fe_m4_sprint_stack[fe_m4_sprint_depth].start_n=b->n; ++fe_m4_sprint_depth; }\n",e->out);
    fputs("fe_writer fe_m4_sprint_writer(void) { return fe_m4_buf_writer(&fe_m4_sprint_stack[fe_m4_sprint_depth-1].b); }\n",e->out);
    fputs("unsigned long fe_m4_sprint_finish(void) { unsigned long result; if (!fe_m4_sprint_depth) abort(); --fe_m4_sprint_depth; result=fe_m4_sprint_stack[fe_m4_sprint_depth].start_n-fe_m4_sprint_stack[fe_m4_sprint_depth].b.n; return result; }\n",e->out);
    fputs("unsigned short fe_m4_write_bytes(fe_writer w, const unsigned char *p, unsigned long n) { return w.write_fn ? w.write_fn(w.ctx,p,n) : 1; }\n",e->out);
    fputs("unsigned short fe_m4_write_cstr(fe_writer w, const char *p) { return fe_m4_write_bytes(w,(const unsigned char*)p,(unsigned long)strlen(p)); }\n",e->out);
    fputs("unsigned short fe_m4_write_str(fe_writer w, fe_str s) { return fe_m4_write_bytes(w,s.p,s.n); }\n",e->out);
    fputs("#define fe_m4_write_slice(w,s) fe_m4_write_bytes((w),(s).p,(s).n)\n",e->out);
    fputs("unsigned short fe_m4_write_int(fe_writer w, long v) { char b[40]; sprintf(b,\"%ld\",v); return fe_m4_write_cstr(w,b); }\n",e->out);
    fputs("unsigned short fe_m4_write_hex(fe_writer w, unsigned long v) { char b[40]; sprintf(b,\"%lx\",v); return fe_m4_write_cstr(w,b); }\n",e->out);
    fputs("unsigned short fe_m4_write_char(fe_writer w, unsigned char v) { return fe_m4_write_bytes(w,&v,1); }\n",e->out);
    fputs("unsigned short fe_m4_write_bool(fe_writer w, unsigned char v) { return fe_m4_write_cstr(w,v ? \"true\" : \"false\"); }\n",e->out);
    fputs("unsigned short fe_m4_write_error(fe_writer w, unsigned long v) { char b[40]; sprintf(b,\"error#%lu\",v); return fe_m4_write_cstr(w,b); }\n",e->out);
}

static int node_uses_m4(FeNode *n)
{
    FeNode *x;
    if (!n) return 0;
    if (n->kind==FE_N_CALL && n->text &&
        (strcmp(n->text,"@print")==0 || strcmp(n->text,"@fprint")==0 ||
         strcmp(n->text,"@sprint")==0)) return 1;
    if (n->kind==FE_N_CALL && n->a && n->a->kind==FE_N_MEMBER &&
        n->a->a && n->a->a->text && strcmp(n->a->a->text,"io")==0) return 1;
    if (node_uses_m4(n->a) || node_uses_m4(n->b) || node_uses_m4(n->c)) return 1;
    for (x=n->children; x; x=x->next) if (node_uses_m4(x)) return 1;
    return 0;
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

static void emit_m4_piece(FILE *out, const char *fmt, unsigned long begin,
                          unsigned long end)
{
    unsigned long i;
    unsigned long cp;
    int h0,h1,h2,h3;
    int c;
    fputc('"',out);
    for(i=begin;i<end;i++) {
        c=(unsigned char)fmt[i];
        if(c=='{' && i+1<end && fmt[i+1]=='{') { fputc('{',out); ++i; continue; }
        if(c=='}' && i+1<end && fmt[i+1]=='}') { fputc('}',out); ++i; continue; }
        if(c=='\\' && i+1<end) {
            ++i; c=(unsigned char)fmt[i];
            if(c=='n') emit_byte(out,10U);
            else if(c=='r') emit_byte(out,13U);
            else if(c=='t') emit_byte(out,9U);
            else if(c=='0') emit_byte(out,0U);
            else if(c=='x' && i+2<end &&
                    (h0=hex_value((unsigned char)fmt[i+1]))>=0 &&
                    (h1=hex_value((unsigned char)fmt[i+2]))>=0) {
                emit_byte(out,(unsigned)((h0<<4)|h1)); i+=2;
            } else if(c=='u' && i+4<end &&
                      (h0=hex_value((unsigned char)fmt[i+1]))>=0 &&
                      (h1=hex_value((unsigned char)fmt[i+2]))>=0 &&
                      (h2=hex_value((unsigned char)fmt[i+3]))>=0 &&
                      (h3=hex_value((unsigned char)fmt[i+4]))>=0) {
                cp=(unsigned long)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                emit_codepoint(out,cp); i+=4;
            }
            else if(c=='\\' || c=='"') { fputc('\\',out); fputc(c,out); }
            else emit_byte(out,(unsigned)c);
            continue;
        }
        if(c=='"' || c=='\\') fputc('\\',out);
        fputc(c,out);
    }
    fputc('"',out);
}

static void emit_m4_writer(FeEmitter *e, FeNode *arg, int buffer)
{
    if (buffer) {
        fputs("fe_m4_buf_writer((fe_m4_slice*)&",e->out);
        if (arg && arg->kind==FE_N_UNARY && arg->text &&
            (strcmp(arg->text,"&")==0 || strcmp(arg->text,"&mut")==0)) emit_expr(e,arg->a);
        else emit_expr(e,arg);
        fputs(")",e->out);
    } else if (arg && arg->kind==FE_N_UNARY && arg->text &&
               (strcmp(arg->text,"&")==0 || strcmp(arg->text,"&mut")==0)) {
        emit_expr(e,arg->a);
    } else if (arg && arg->kind==FE_N_CALL && arg->a &&
               arg->a->kind==FE_N_MEMBER) {
        emit_expr(e,arg);
    } else if (arg && arg->sem_type && arg->sem_type->kind==FE_TYPE_REF) {
        fputs("(*",e->out); emit_expr(e,arg); fputc(')',e->out);
    } else {
        emit_expr(e,arg);
    }
}

static void emit_m4_writer_value(FeEmitter *e, FeNode *writer, int buffer)
{
    if (!writer) fputs("fe_m4_stdout_writer()",e->out);
    else if (buffer) fputs("fe_m4_sprint_writer()",e->out);
    else emit_m4_writer(e,writer,buffer);
}

static void emit_m4_arg(FeEmitter *e, FeNode *arg, int verb,
                        FeNode *writer, int buffer, int error_value)
{
    FeType *t=arg ? arg->sem_type : 0;
    if (verb=='x') {
        fputs("fe_m4_write_hex(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", (unsigned long)",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    if (verb=='c') {
        fputs("fe_m4_write_char(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", (unsigned char)",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    if (verb=='b') {
        fputs("fe_m4_write_bool(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", ",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    if (verb=='s' || (verb==' ' && t && (t->kind==FE_TYPE_STR || t->kind==FE_TYPE_SLICE))) {
        if (t && t->kind==FE_TYPE_STR) fputs("fe_m4_write_str(",e->out);
        else fputs("fe_m4_write_slice(",e->out);
        emit_m4_writer_value(e,writer,buffer); fputs(", ",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    if (error_value) {
        fputs("fe_m4_write_error(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", (unsigned long)",e->out); emit_expr(e,arg); fputs(".tag)",e->out); return;
    }
    if (verb==' ' && t && t->kind==FE_TYPE_BOOL) {
        fputs("fe_m4_write_bool(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", ",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    if (verb==' ' && t && t->kind==FE_TYPE_CHAR) {
        fputs("fe_m4_write_char(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", (unsigned char)",e->out); emit_expr(e,arg); fputc(')',e->out); return;
    }
    fputs("fe_m4_write_int(",e->out); emit_m4_writer_value(e,writer,buffer); fputs(", (long)",e->out); emit_expr(e,arg); fputc(')',e->out);
}

static void emit_m4_builtin(FeEmitter *e, FeNode *n)
{
    const char *fmt=n->aux_text;
    FeNode *fmt_node=n->children;
    FeNode *arg;
    FeNode *writer_arg=0;
    FeNode *buffer_arg=0;
    unsigned long i,j,last=1;
    unsigned count=0;
    int verb;
    int error_value;
    int first=1;
    int is_print=strcmp(n->text,"@print")==0;
    int is_sprint=strcmp(n->text,"@sprint")==0;
    if (!fmt) { fputs("0",e->out); return; }
    if (is_print) writer_arg=0;
    else if (is_sprint) { buffer_arg=fmt_node; fmt_node=fmt_node ? fmt_node->next : 0; }
    else { writer_arg=fmt_node; fmt_node=fmt_node ? fmt_node->next : 0; }
    arg=fmt_node ? fmt_node->next : 0;
    error_value=0;
    fputc('(',e->out);
    if (!is_print && !is_sprint) {
        fputs("fe_m4_error=0",e->out);
        first=0;
    }
    if (is_sprint) {
        fputs("fe_m4_sprint_begin((fe_m4_slice*)&",e->out); emit_expr(e,buffer_arg); fputs(")",e->out);
        first=0;
    }
    i=1;
    while(fmt[i] && fmt[i]!='"') {
        if(fmt[i]=='\\') { ++i; if(fmt[i]) ++i; continue; }
        if(fmt[i]=='{' && fmt[i+1]=='{') { i+=2; continue; }
        if(fmt[i]=='}' && fmt[i+1]=='}') { i+=2; continue; }
        if(fmt[i]=='{') {
            j=i+1; while(fmt[j] && fmt[j]!='}') ++j;
            if(!fmt[j]) break;
            if(i>last) {
                if(!first) fputs(", ",e->out);
                if (!is_print && !is_sprint)
                    fputs("fe_m4_error ? fe_m4_error : (fe_m4_error = ",e->out);
                if(is_print) { fputs("fe_m4_write_cstr(fe_m4_stdout_writer(), ",e->out); emit_m4_piece(e->out,fmt,last,i); fputc(')',e->out); }
                else { fputs("fe_m4_write_cstr(",e->out); if(is_sprint) emit_m4_writer_value(e,buffer_arg,1); else emit_m4_writer(e,writer_arg,0); fputs(", ",e->out); emit_m4_piece(e->out,fmt,last,i); fputc(')',e->out); }
                if (!is_print && !is_sprint) fputc(')',e->out);
                first=0;
            }
            verb=(j==i+1) ? ' ' : (j==i+2 ? (unsigned char)fmt[i+1] : '?');
            if(arg) {
                error_value=arg->sem_type && arg->sem_type->kind==FE_TYPE_ENUM &&
                    arg->sem_type->is_error;
                if(!first) fputs(", ",e->out);
                if (!is_print && !is_sprint)
                    fputs("fe_m4_error ? fe_m4_error : (fe_m4_error = ",e->out);
                if(is_print) emit_m4_arg(e,arg,verb,0,0,error_value);
                else {
                    /* The writer expression is repeated intentionally; it is
                       a value wrapper and does not re-evaluate source args. */
                    emit_m4_arg(e,arg,verb, is_sprint ? buffer_arg : writer_arg,
                                is_sprint,error_value);
                }
                if (!is_print && !is_sprint) fputc(')',e->out);
                first=0; arg=arg->next; ++count;
            }
            last=j+1; i=j+1; continue;
        }
        ++i;
    }
    if(fmt[i]=='"' && i>last) {
        if(!first) fputs(", ",e->out);
        if (!is_print && !is_sprint)
            fputs("fe_m4_error ? fe_m4_error : (fe_m4_error = ",e->out);
        if(is_print) { fputs("fe_m4_write_cstr(fe_m4_stdout_writer(), ",e->out); emit_m4_piece(e->out,fmt,last,i); fputc(')',e->out); }
        else { fputs("fe_m4_write_cstr(",e->out); if(is_sprint) emit_m4_writer_value(e,buffer_arg,1); else emit_m4_writer(e,writer_arg,0); fputs(", ",e->out); emit_m4_piece(e->out,fmt,last,i); fputc(')',e->out); }
        if (!is_print && !is_sprint) fputc(')',e->out);
        first=0;
    }
    if(first) fputs("0",e->out);
    if(is_print) fputs(", (void)0",e->out);
    else if(is_sprint) { fputs(", fe_m4_sprint_finish()",e->out); }
    fputc(')',e->out);
    (void)count;
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
        } else if (n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_OWNED &&
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

static void emit_destroy_expr(FeEmitter *e, FeNode *n)
{
    fputs("(free(",e->out); emit_expr(e,n); fputs(")",e->out);
    if (n && n->kind==FE_N_IDENT) {
        fputs(", ",e->out); emit_lvalue(e,n);
        fputs("=0, fe_live_",e->out); fputs(cname(n,"owned"),e->out);
        fputs("=0",e->out);
    } else {
        fputs(", ",e->out); emit_lvalue(e,n); fputs("=0",e->out);
    }
    fputs(", 0)",e->out);
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
        fputs(bt->full_slicer,e->out);
        if (bt->kind==FE_TYPE_ARRAY) { fputs("(&",e->out); emit_lvalue(e,n->a); }
        else { fputc('(',e->out); emit_expr(e,n->a); }
        fputc(')',e->out); return;
    }
    if (!n->c && bt && bt->tail_slicer) {
        fputs(bt->tail_slicer,e->out);
        if (bt->kind==FE_TYPE_ARRAY) { fputs("(&",e->out); emit_lvalue(e,n->a); }
        else { fputc('(',e->out); emit_expr(e,n->a); }
        fputs(", ",e->out);
        if (n->b) emit_expr(e,n->b); else fputs("0",e->out);
        fputc(')',e->out); return;
    }
    fputs(maker,e->out);
    if (bt && bt->kind==FE_TYPE_ARRAY) { fputs("(&",e->out); emit_lvalue(e,n->a); }
    else { fputc('(',e->out); emit_expr(e,n->a); }
    fputs(", ",e->out);
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
        if (strcmp(op, "try") == 0) {
            if (n->a && n->a->sem_type &&
                n->a->sem_type->kind==FE_TYPE_ERROR_UNION &&
                n->a->sem_type->error_value &&
                n->a->sem_type->error_value->kind!=FE_TYPE_VOID) {
                fputc('(',e->out); emit_expr(e,n->a); fputs(").v",e->out);
            } else emit_expr(e,n->a);
            break;
        }
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
        if(n->text && (strcmp(n->text,"@print")==0 || strcmp(n->text,"@fprint")==0 || strcmp(n->text,"@sprint")==0)) { emit_m4_builtin(e,n); special=1; }
        else if(n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
                n->a->a->kind==FE_N_IDENT && n->a->a->text &&
                strcmp(n->a->a->text,"mem")==0 && n->a->b &&
                n->a->b->text && strcmp(n->a->b->text,"destroy")==0 &&
                n->children) {
            emit_destroy_expr(e,n->children);
            special=1;
        }
        else if(n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
                n->a->a->kind==FE_N_IDENT && n->a->a->text &&
                strcmp(n->a->a->text,"mem")==0 && n->a->b &&
                n->a->b->text && strcmp(n->a->b->text,"create")==0 &&
                n->children && n->children->kind==FE_N_IDENT) {
            FeType *created=fe_type_intern(&e->check->types,n->children->text);
            FeType *owned=fe_type_owned(&e->check->types,created);
            FeType *result=fe_type_error_union(&e->check->types,owned);
            if (result->alloc_cname) fputs(result->alloc_cname,e->out);
            else fputs("fe_bad_alloc",e->out);
            fputs("()",e->out);
            special=1;
        }
        else if(n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
                n->a->a->kind==FE_N_IDENT && n->a->a->text &&
                strcmp(n->a->a->text,"io")==0 && n->a->b && n->a->b->text &&
                strcmp(n->a->b->text,"buf_writer")==0 && n->children) { emit_m4_writer(e,n->children,1); special=1; }
        else if(n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
                n->a->a->kind==FE_N_IDENT && n->a->a->text &&
                strcmp(n->a->a->text,"io")==0 && n->a->b && n->a->b->text &&
                strcmp(n->a->b->text,"null_writer")==0) { fputs("fe_m4_null_writer()",e->out); special=1; }
        else if(!n->a && n->text && strcmp(n->text,"@size_of")==0 && n->children && n->children->kind==FE_N_IDENT) { fprintf(e->out,"%lu",fe_type_size(fe_type_intern(&e->check->types,n->children->text))); special=1; }
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
                if ((x->flags & 0x100U) && x->kind==FE_N_IDENT &&
                    x->sem_type && x->sem_type->kind==FE_TYPE_OWNED) {
                    fputs("(fe_live_",e->out); fputs(cname(x,"owned"),e->out);
                    fputs("=0, ",e->out); emit_expr(e,x); fputc(')',e->out);
                } else emit_expr(e, x);
            }
            fputc(')', e->out);
        }
        break;
    }
    case FE_N_MEMBER: {
        FeVariantType *v;
        if(n->a && n->a->kind==FE_N_IDENT && n->a->text &&
           strcmp(n->a->text,"io")==0 && n->b && n->b->text &&
           strcmp(n->b->text,"stdout")==0) fputs("fe_m4_stdout_writer()",e->out);
        else if(n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_REF &&
           n->b && n->b->text && strcmp(n->b->text,"^")==0) {
            fputs("(*",e->out); emit_expr(e,n->a); fputs(")",e->out);
        } else if(n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_OWNED &&
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
    if (n->kind==FE_N_CONST && n->b) {
        fputs(" = ",e->out);
        if (n->b->kind==FE_N_LITERAL && n->b->text && n->b->text[0]=='"') {
            fputs("{ (const unsigned char*)",e->out);
            emit_c_literal(e->out,n->b->text,1);
            fputs(", sizeof(",e->out);
            emit_c_literal(e->out,n->b->text,1);
            fputs(")-1 }",e->out);
        } else emit_expr(e,n->b);
    }
    fputs(";\n", e->out);
    if ((n->kind==FE_N_LET || n->kind==FE_N_VAR) && n->sem_type &&
        n->sem_type->kind==FE_TYPE_OWNED) {
        pad(e); fputs("unsigned char fe_live_",e->out);
        fputs(cname(n,"owned"),e->out); fputs("=0;\n",e->out);
    }
}

static void emit_owned_live(FeEmitter *e, FeNode *n, int value)
{
    if (n && n->sem_type && n->sem_type->kind==FE_TYPE_OWNED) {
        pad(e); fputs("fe_live_",e->out); fputs(cname(n,"owned"),e->out);
        fprintf(e->out,"=%d;\n",value);
    }
}

static void emit_value_drop(FeEmitter *e, FeNode *n)
{
    FeType *t=n ? n->sem_type : 0;
    if (!n || !t || !type_needs_drop(t) || (n->flags & 0x100U) ||
        (n->flags & 0x200U)) return;
    if (t->kind==FE_TYPE_OWNED) {
        pad(e); fputs("if (fe_live_",e->out); fputs(cname(n,"owned"),e->out);
        fputs(") { ",e->out);
        if (t->elem && type_needs_drop(t->elem) && t->elem->drop_cname) {
            fprintf(e->out,"%s(%s); ",t->elem->drop_cname,cname(n,"owned"));
        }
        fputs("free(",e->out); fputs(cname(n,"owned"),e->out);
        fputs("); fe_live_",e->out); fputs(cname(n,"owned"),e->out);
        fputs("=0; }\n",e->out);
    } else if (t->drop_cname) {
        pad(e); fprintf(e->out,"%s(&%s);\n",t->drop_cname,cname(n,"local"));
    }
}

static void emit_cleanup_block(FeEmitter *e, FeNode *n)
{
    FeNode *x;
    unsigned count=0;
    unsigned index;
    unsigned seen=0xffffffffU;
    unsigned depth;
    /* A defer becomes active only after its declaration statement was
       reached.  In particular, a failing initializer must not run a later
       defer merely because it shares this AST block. */
    for (depth=0; depth<e->block_depth; ++depth)
        if (e->block_stack[depth]==n) { seen=e->block_seen[depth]; break; }
    for (x=n ? n->children : 0, index=0; x; x=x->next, ++index)
        if (index<seen &&
            (x->kind==FE_N_DEFER || x->kind==FE_N_LET || x->kind==FE_N_VAR)) ++count;
    while (count) {
        index=0;
        for (x=n->children; x; x=x->next)
            if ((x->kind==FE_N_DEFER || x->kind==FE_N_LET || x->kind==FE_N_VAR) &&
                index++==count-1) {
                if (x->kind==FE_N_DEFER) emit_stmt(e,x->a); else emit_value_drop(e,x);
                break;
            }
        --count;
    }
}

static void emit_cleanup_to(FeEmitter *e, unsigned floor)
{
    unsigned i;
    for (i=e->block_depth; i>floor; --i) emit_cleanup_block(e,e->block_stack[i-1]);
}

static void emit_param_cleanup(FeEmitter *e)
{
    FeNode *p;
    if (!e->current_fn || !e->current_fn->a) return;
    for (p=e->current_fn->a->children; p; p=p->next) emit_value_drop(e,p);
}

static void emit_cleanup_all(FeEmitter *e)
{
    emit_cleanup_to(e,0);
    emit_param_cleanup(e);
}

static void emit_error_return(FeEmitter *e, const char *error_expr)
{
    if (e->current_ret && e->current_ret->kind==FE_TYPE_ERROR_UNION &&
        e->current_ret->error_value && e->current_ret->error_value->kind!=FE_TYPE_VOID) {
        fputs("return ",e->out); fputs(e->current_ret->maker,e->out);
        fputs("(",e->out); fputs(error_expr,e->out); fputs(", (",e->out);
        fputs(fe_type_c_name(e->current_ret->error_value,e->pointer_bits),e->out);
        fputs(")0);\n",e->out);
    } else {
        fputs("return ",e->out); fputs(error_expr,e->out); fputs(";\n",e->out);
    }
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
    if (e->block_depth<32U) {
        e->block_stack[e->block_depth]=n;
        e->block_seen[e->block_depth]=0;
        ++e->block_depth;
    }
    /* C89 requires declarations before statements in each actual block. */
    for (x = n->children; x; x = x->next)
        if (x->kind == FE_N_LET || x->kind == FE_N_VAR ||
            x->kind == FE_N_CONST) emit_decl(e, x);
    if (e->current_fn && e->current_fn->c==n && e->current_fn->a) {
        FeNode *param;
        for (param=e->current_fn->a->children; param; param=param->next)
            if (param->sem_type && param->sem_type->kind==FE_TYPE_OWNED) {
                pad(e); fputs("unsigned char fe_live_",e->out);
                fputs(cname(param,"owned"),e->out); fputs("=1;\n",e->out);
            }
    }
    if (e->current_ret && e->current_ret->kind!=FE_TYPE_VOID) {
        pad(e); fputs(fe_type_c_name(e->current_ret,e->pointer_bits),e->out);
        fputs(" fe_return_value;\n",e->out);
    }
    {
        unsigned seen=0;
        for (x = n->children; x; x = x->next) {
            ++seen;
            if (e->block_depth) e->block_seen[e->block_depth-1]=seen;
            emit_stmt(e, x);
        }
    }
    --e->indent;
    emit_cleanup_block(e,n);
    if (e->current_fn && e->current_fn->c==n) emit_param_cleanup(e);
    if (e->block_depth) --e->block_depth;
    if (e->fallthrough_block==n) {
        pad(e);
        fputs("return 0;\n",e->out);
        e->fallthrough_block=0;
    }
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

static int try_has_value(FeNode *n)
{
    return n && n->kind==FE_N_UNARY && n->text && strcmp(n->text,"try")==0 &&
        n->a && n->a->sem_type && n->a->sem_type->kind==FE_TYPE_ERROR_UNION &&
        n->a->sem_type->error_value && n->a->sem_type->error_value->kind!=FE_TYPE_VOID;
}

static void emit_try_statement(FeEmitter *e, FeNode *try_node, FeNode *target)
{
    char temp[40];
    char error[48];
    FeType *result=try_node->a->sem_type;
    sprintf(temp,"fe_try_%u",e->temp_serial++);
    sprintf(error,"%s.e",temp);
    pad(e); fputs("{ ",e->out); fputs(fe_type_c_name(result,e->pointer_bits),e->out);
    fputc(' ',e->out); fputs(temp,e->out); fputs(" = ",e->out);
    emit_expr(e,try_node->a); fputs("; if (",e->out); fputs(temp,e->out);
    fputs(".e) {\n",e->out); ++e->indent;
    emit_cleanup_all(e);
    pad(e); emit_error_return(e,error);
    --e->indent; pad(e); fputs("} ",e->out);
    if (target) {
        if (target->kind==FE_N_LET || target->kind==FE_N_VAR)
            fputs(cname(target,"fe_local"),e->out);
        else
            emit_lvalue(e,target);
        fputs(" = ",e->out); fputs(temp,e->out); fputs(".v;\n",e->out);
    }
    fputs("}\n",e->out);
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
            if (try_has_value(n->b)) emit_try_statement(e,n->b,n);
            else {
                pad(e);
                fputs(cname(n, "fe_local"), e->out);
                fputs(" = ", e->out);
                emit_expr(e, n->b);
                fputs(";\n", e->out);
            }
            emit_owned_live(e,n,1);
        }
        break;
    case FE_N_ASSIGN:
        if (n->a && n->a->kind==FE_N_IDENT && n->a->sem_type &&
            n->a->sem_type->kind==FE_TYPE_OWNED) {
            pad(e); fputs("if (fe_live_",e->out); fputs(cname(n->a,"owned"),e->out);
            fputs(") { ",e->out);
            if (n->a->sem_type->elem && type_needs_drop(n->a->sem_type->elem) &&
                n->a->sem_type->elem->drop_cname)
                fprintf(e->out,"%s(%s); ",n->a->sem_type->elem->drop_cname,cname(n->a,"owned"));
            fputs("free(",e->out); fputs(cname(n->a,"owned"),e->out);
            fputs("); fe_live_",e->out); fputs(cname(n->a,"owned"),e->out);
            fputs("=0; }\n",e->out);
        }
        pad(e);
        emit_lvalue(e, n->a);
        fputc(' ', e->out);
        fputs(n->text ? n->text : "=", e->out);
        fputs(" ", e->out);
        if (n->b && (n->b->flags & 0x100U) && n->b->kind==FE_N_IDENT &&
            n->b->sem_type && n->b->sem_type->kind==FE_TYPE_OWNED) {
            fputs("(fe_live_",e->out); fputs(cname(n->b,"owned"),e->out);
            fputs("=0, ",e->out); emit_expr(e,n->b); fputc(')',e->out);
        } else emit_expr(e, n->b);
        fputs(";\n", e->out);
        if (n->a && n->a->kind==FE_N_IDENT) emit_owned_live(e,n->a,1);
        break;
    case FE_N_EXPR_STMT:
        if (try_has_value(n->a)) {
            emit_try_statement(e,n->a,0);
        } else {
            pad(e);
            if (n->a && n->a->kind==FE_N_UNARY && n->a->text &&
                strcmp(n->a->text,"try")==0 && n->a->a) {
                fputs("if ((fe_m4_error = ",e->out);
                emit_expr(e,n->a->a);
                fputs(") != 0) {\n",e->out);
                ++e->indent;
                emit_cleanup_all(e);
                pad(e); fputs("return fe_m4_error;\n",e->out);
                --e->indent;
                pad(e); fputs("}\n",e->out);
            } else {
                emit_expr(e, n->a);
                fputs(";\n", e->out);
            }
        }
        break;
    case FE_N_BREAK:
    case FE_N_CONTINUE:
        if (e->loop_depth) {
            emit_cleanup_to(e,e->loop_floor[e->loop_depth-1]);
            pad(e); fputs(n->kind==FE_N_BREAK ? "break;\n" : "continue;\n",e->out);
        }
        break;
    case FE_N_RETURN:
        if (n->a && n->a->kind == FE_N_MATCH) {
            emit_match(e,n->a,1);
            break;
        }
        /* Evaluate before cleanup: `return p.^` must not dereference p after
           its owned cleanup has run.  The block-local temporary is declared
           before all statements to retain C89 declaration ordering. */
        if (n->a && e->current_ret && e->current_ret->kind!=FE_TYPE_VOID) {
            pad(e); fputs("fe_return_value = ",e->out);
            if ((n->a->flags & 0x100U) && n->a->kind==FE_N_IDENT &&
                n->a->sem_type && n->a->sem_type->kind==FE_TYPE_OWNED) {
                fputs("(fe_live_",e->out); fputs(cname(n->a,"owned"),e->out);
                fputs("=0, ",e->out); emit_expr(e,n->a); fputc(')',e->out);
            } else emit_expr(e,n->a);
            fputs(";\n",e->out);
        }
        emit_cleanup_all(e);
        pad(e); fputs("return",e->out);
        if (n->a) fputs(" fe_return_value",e->out);
        fputs(";\n",e->out);
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
        if (e->loop_depth<16U) e->loop_floor[e->loop_depth++]=e->block_depth;
        if (n->b && n->b->kind == FE_N_BLOCK) emit_block(e, n->b);
        else emit_block(e, 0);
        if (e->loop_depth) --e->loop_depth;
        fputc('\n', e->out);
        break;
    case FE_N_FOR:
        pad(e); fputs("{\n",e->out); ++e->indent;
        if (e->loop_depth<16U) e->loop_floor[e->loop_depth++]=e->block_depth;
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
        --e->indent;
        if (e->loop_depth) --e->loop_depth;
        pad(e); fputs("}\n",e->out); break;
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
        FeType *old_ret=e->current_ret;
        FeNode *old_fn=e->current_fn;
        e->current_ret=fn->sem_type;
        e->current_fn=fn;
        fputs(" ", e->out);
        if (fn->sem_type && fn->sem_type->kind==FE_TYPE_ERROR_UNION &&
            fn->sem_type->error_value &&
            fn->sem_type->error_value->kind==FE_TYPE_VOID)
            e->fallthrough_block=fn->c;
        emit_block(e, fn->c);
        e->current_ret=old_ret;
        e->current_fn=old_fn;
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
    e->fallthrough_block = 0;
    e->block_depth = 0;
    e->loop_depth = 0;
    e->current_ret = 0;
    e->current_fn = 0;
}

void fe_emit_c_program(FeEmitter *e)
{
    FeNode *n;
    FeNode *main_fn = 0;
    FeType *type;
    int need_m4;
    need_m4=node_uses_m4(e->check->ast->root);
    for (type=e->check->types.types; type; type=type->next)
        if (strcmp(type->name,"io.Writer")==0) need_m4=1;
    fputs("/* generated by fec M4 */\n#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\ntypedef char fe_assert_u8[(sizeof(unsigned char)==1) ? 1 : -1];\ntypedef char fe_assert_u16[(sizeof(unsigned short)==2) ? 1 : -1];\ntypedef char fe_assert_u32[(sizeof(unsigned long)==4) ? 1 : -1];\n", e->out);
    if (e->pointer_bits==16)
        fputs("typedef char fe_assert_usize[(sizeof(unsigned short)==2) ? 1 : -1];\n",e->out);
    else
        fputs("typedef char fe_assert_usize[(sizeof(unsigned long)==4) ? 1 : -1];\n",e->out);
    fputs("static void fe_trap_bounds(void) { abort(); }\n\n", e->out);
    emit_type_defs(e);
    if (need_m4) emit_m4_runtime(e);
    emit_type_helpers(e);
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            fputs(ctype(e, n), e->out);
            fputc(' ', e->out);
            fputs(cname(n, "fe_global"), e->out);
            if (n->b) {
                fputs(" = ", e->out);
                if (n->kind==FE_N_CONST && n->b->kind==FE_N_LITERAL &&
                    n->b->text && n->b->text[0]=='"') {
                    fputs("{ (const unsigned char*)",e->out);
                    emit_c_literal(e->out,n->b->text,1);
                    fputs(", sizeof(",e->out);
                    emit_c_literal(e->out,n->b->text,1);
                    fputs(")-1 }",e->out);
                } else emit_expr(e, n->b);
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
