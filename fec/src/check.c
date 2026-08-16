#include "check.h"
#include <string.h>
#include <stdio.h>

typedef struct FeSym FeSym;
typedef struct FeScope FeScope;

struct FeSym {
    const char *name;
    char *cname;
    FeType *type;
    FeNode *fn;
    int mutable;
    int initialized;
    int moved;
    FeNode *decl;
};

struct FeScope {
    FeScope *parent;
    FeSym *items;
    unsigned count;
    unsigned capacity;
};

static FeSym *find_symbol(FeScope *scope, const char *name);

typedef struct FeCheckerState {
    FeCheck *c;
    FeScope *scope;
    FeScope *globals;
    FeType *ret;
    unsigned loop_depth;
    unsigned defer_depth;
} FeCheckerState;

static FeType *unknown(FeCheck *c)
{
    return fe_type_intern(&c->types, "<unknown>");
}

static void err(FeCheck *c, FeLoc loc, const char *msg)
{
    fe_diag_error(c->diags, loc, msg);
}

static int known(FeType *t)
{
    return t && t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_ERROR;
}

static int is_copy_type(FeType *t)
{
    unsigned i;
    if (!t) return 1;
    if (t->kind==FE_TYPE_OWNED) return 0;
    if (t->kind==FE_TYPE_REF || t->kind==FE_TYPE_SLICE) return !t->ref_mut;
    if (t->kind==FE_TYPE_ARRAY) return is_copy_type(t->elem);
    if (t->kind==FE_TYPE_STRUCT) {
        if (t->has_drop) return 0;
        for (i=0;i<t->field_count;i++) if (!is_copy_type(t->fields[i].type)) return 0;
    }
    if (t->kind==FE_TYPE_ENUM)
        for (i=0;i<t->variant_count;i++) {
            unsigned j;
            for (j=0;j<t->variants[i].field_count;j++)
                if (!is_copy_type(t->variants[i].fields[j].type)) return 0;
        }
    return 1;
}

static void mark_moved(FeCheckerState *s, FeNode *n, FeType *t)
{
    FeSym *sym;
    if (!n || !t || is_copy_type(t)) return;
    if(n->kind==FE_N_INDEX && t->kind==FE_TYPE_SLICE &&
       (n->c || !n->b)) return;
    if(n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX) {
        err(s->c,n->loc,
            "cannot move a non-Copy value out of a projection; use mem.replace");
        return;
    }
    if(n->kind!=FE_N_IDENT) return;
    sym=find_symbol(s->scope,n->text ? n->text : "");
    if (sym) {
        if (s->defer_depth) {
            if (sym->decl) sym->decl->flags |= 0x200U;
        } else {
            sym->moved=1;
            /* Mark this consuming expression, not the declaration. Branches
               may move conditionally; the declaration's runtime live flag
               must remain available to guard cleanup on the other path. */
            n->flags |= 0x100U;
        }
    }
}

static int compatible(FeType *want, FeType *got, FeNode *value)
{
    FeNode *item;
    unsigned long count;
    if (want && got && value && value->kind==FE_N_ARRAY_INIT &&
        want->kind==FE_TYPE_ARRAY && got->kind==FE_TYPE_ARRAY) {
        if (want->length != got->length) return 0;
        count=0;
        for (item=value->children; item; item=item->next) {
            if (!compatible(want->elem,item->sem_type,item)) return 0;
            if (item->kind==FE_N_LITERAL && item->text &&
                fe_type_is_integer(want->elem) &&
                fe_type_is_integer(item->sem_type) &&
                item->text[0]!='\'' && item->text[0]!='"')
                item->sem_type=want->elem;
            ++count;
        }
        if (count!=want->length) return 0;
        value->sem_type=want;
        return 1;
    }
    if (fe_type_equal(want, got)) return 1;
    if (!known(want) || !known(got)) return 1;
    return fe_type_is_integer(want) && fe_type_is_integer(got) && value &&
        value->kind == FE_N_LITERAL && value->text &&
        value->text[0] != '\'' && value->text[0] != '"';
}

static int explicit_castable(FeType *a, FeType *b)
{
    if (!a || !b) return 0;
    return (fe_type_is_integer(a) || a->kind == FE_TYPE_CHAR) &&
           (fe_type_is_integer(b) || b->kind == FE_TYPE_CHAR);
}

static FeType *node_type(FeCheck *c, FeNode *n)
{
    FeType *t;
    if (!n) return unknown(c);
    t = fe_type_from_ast(&c->types, n);
    n->sem_type = t;
    return t;
}

static char *unit_cname(FeCheck *c, const char *name)
{
    char *u;
    char *p;
    unsigned long n;
    u = c->ast->root && c->ast->root->text ? c->ast->root->text : "unit";
    n = (unsigned long)strlen("fe_") + (unsigned long)strlen(u) +
        (unsigned long)strlen(name ? name : "name") + 2UL;
    p = (char *)fe_arena_alloc(&c->ast->arena, n);
    if (!p) return 0;
    strcpy(p, "fe_");
    strcat(p, u);
    strcat(p, "_");
    strcat(p, name ? name : "name");
    return p;
}

static char *local_cname(FeCheck *c, const char *name)
{
    char number[24];
    char *p;
    unsigned long n;
    sprintf(number, "%u", c->local_serial++);
    n = (unsigned long)strlen("fe_l_") + (unsigned long)strlen(name) +
        (unsigned long)strlen(number) + 2UL;
    p = (char *)fe_arena_alloc(&c->ast->arena, n);
    if (!p) return 0;
    strcpy(p, "fe_l_");
    strcat(p, name ? name : "local");
    strcat(p, "_");
    strcat(p, number);
    return p;
}

static FeScope *scope_new(FeCheckerState *s, FeScope *parent)
{
    FeScope *scope;
    scope = (FeScope *)fe_arena_alloc(&s->c->ast->arena, sizeof(FeScope));
    if (!scope) {
        err(s->c, s->c->ast->root->loc, "out of memory creating scope");
        return parent;
    }
    scope->parent = parent;
    scope->items = 0;
    scope->count = 0;
    scope->capacity = 0;
    return scope;
}

static FeSym *find_current(FeScope *scope, const char *name)
{
    unsigned i;
    if (!scope) return 0;
    for (i = scope->count; i > 0; --i)
        if (strcmp(scope->items[i - 1].name, name) == 0)
            return &scope->items[i - 1];
    return 0;
}

static FeSym *find_symbol(FeScope *scope, const char *name)
{
    FeSym *sym;
    while (scope) {
        sym = find_current(scope, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return 0;
}

static FeSym *add_symbol(FeCheckerState *s, FeScope *scope,
                         const char *name, FeType *type, FeNode *fn,
                         int mutable, int initialized, char *cname,
                         FeNode *decl)
{
    FeSym *items;
    unsigned capacity;
    FeSym *sym;
    if (!name) name = "<unnamed>";
    if (find_current(scope, name)) {
        err(s->c, decl ? decl->loc : s->c->ast->root->loc,
            "duplicate declaration in scope");
        return 0;
    }
    if (scope->count == scope->capacity) {
        capacity = scope->capacity ? scope->capacity * 2U : 8U;
        items = (FeSym *)fe_arena_alloc(&s->c->ast->arena,
                                        capacity * sizeof(FeSym));
        if (!items) {
            err(s->c, decl ? decl->loc : s->c->ast->root->loc,
                "out of memory growing symbol scope");
            return 0;
        }
        if (scope->items)
            memcpy(items, scope->items, scope->count * sizeof(FeSym));
        scope->items = items;
        scope->capacity = capacity;
    }
    sym = &scope->items[scope->count++];
    sym->name = name;
    sym->cname = cname;
    sym->type = type;
    sym->fn = fn;
    sym->mutable = mutable;
    sym->initialized = initialized;
    sym->moved = 0;
    sym->decl = decl;
    if (decl) {
        decl->cname = cname;
        decl->sem_type = type;
    }
    return sym;
}

void fe_check_init(FeCheck *c, FeAst *ast, FeDiags *diags,
                   unsigned pointer_bits, int no_checks)
{
    c->ast = ast;
    c->diags = diags;
    c->pointer_bits = pointer_bits;
    c->local_serial = 0;
    c->no_checks = no_checks;
    fe_types_init(&c->types, &ast->arena, pointer_bits);
    c->types.unit_name = ast->root && ast->root->text ? ast->root->text : "unit";
}

static FeType *check_expr(FeCheckerState *s, FeNode *n);

static FeNode *find_method(FeCheck *c, FeType *owner, const char *name)
{
    FeNode *decl;
    FeNode *method;
    if(!owner || !name) return 0;
    for(decl=c->ast->root ? c->ast->root->children : 0; decl; decl=decl->next)
        if(decl->kind==FE_N_STRUCT && decl->text &&
           strcmp(decl->text,owner->name)==0)
            for(method=decl->children; method; method=method->next)
                if(method->kind==FE_N_FN && method->text &&
                   strcmp(method->text,name)==0) return method;
    return 0;
}

static FeType *method_type(FeCheck *c, FeNode *node, FeType *owner)
{
    if(node && node->kind==FE_N_TYPE && node->text &&
       strcmp(node->text,"Self")==0) return owner;
    if(node && node->kind==FE_N_TYPE && node->text &&
       (strcmp(node->text,"&")==0 || strcmp(node->text,"&mut")==0) &&
       node->a && node->a->text && strcmp(node->a->text,"Self")==0)
        return fe_type_ref(&c->types,owner,strcmp(node->text,"&mut")==0);
    return node_type(c,node);
}
static void check_match(FeCheckerState *s, FeNode *n);
static void check_stmt(FeCheckerState *s, FeNode *n);

typedef struct FeFlowSlot {
    FeSym *sym;
    int moved;
    int initialized;
} FeFlowSlot;

static unsigned flow_capture(FeScope *scope, FeFlowSlot *slots, unsigned cap)
{
    unsigned count=0;
    unsigned i;
    FeScope *p;
    for (p=scope; p && count<cap; p=p->parent)
        for (i=0; i<p->count && count<cap; ++i) {
            slots[count].sym=&p->items[i];
            slots[count].moved=p->items[i].moved;
            slots[count].initialized=p->items[i].initialized;
            ++count;
        }
    return count;
}

static void flow_restore(FeFlowSlot *slots, unsigned count)
{
    unsigned i;
    for (i=0; i<count; ++i) {
        slots[i].sym->moved=slots[i].moved;
        slots[i].sym->initialized=slots[i].initialized;
    }
}

static void flow_merge(FeFlowSlot *base, FeFlowSlot *left, FeFlowSlot *right,
                       unsigned count)
{
    unsigned i;
    for (i=0; i<count; ++i) {
        base[i].sym->moved = left[i].moved==1 && right[i].moved==1 ? 1 :
            (left[i].moved || right[i].moved ? 2 : 0);
        base[i].sym->initialized = left[i].initialized && right[i].initialized;
    }
}

static FeNode *find_const_node(FeCheck *c, const char *name)
{
    FeNode *n;
    for (n=c->ast->root ? c->ast->root->children : 0; n; n=n->next)
        if (n->kind==FE_N_CONST && n->text && name && strcmp(n->text,name)==0)
            return n;
    return 0;
}

static int format_is_slice_u8(FeType *t);

static const char *builtin_format(FeCheckerState *s, FeNode *fmt)
{
    FeNode *decl;
    FeSym *sym;
    if (fmt && fmt->kind==FE_N_LITERAL && fmt->text && fmt->text[0]=='"')
        return fmt->text;
    if (fmt && fmt->kind==FE_N_IDENT) {
        sym=find_symbol(s->scope,fmt->text);
        decl=sym && sym->decl && sym->decl->kind==FE_N_CONST ?
            sym->decl : find_const_node(s->c,fmt->text);
        if (decl && decl->b && decl->b->kind==FE_N_LITERAL &&
            decl->b->text && decl->b->text[0]=='"') {
            if (!decl->a || format_is_slice_u8(fe_type_from_ast(&s->c->types,decl->a)))
                return decl->b->text;
        }
    }
    return 0;
}

static int format_is_slice_u8(FeType *t)
{
    return t && t->kind==FE_TYPE_SLICE && t->elem &&
           t->elem->kind==FE_TYPE_INT && strcmp(t->elem->name,"u8")==0;
}

static int format_is_writer_type(FeType *t)
{
    return t && t->kind==FE_TYPE_STRUCT &&
        (strcmp(t->name,"Writer")==0 || strcmp(t->name,"io.Writer")==0);
}

static int format_arg_ok(FeType *t, int verb)
{
    if (!t) return 0;
    if (verb=='x') return fe_type_is_integer(t);
    if (verb=='c') return t->kind==FE_TYPE_CHAR;
    if (verb=='s') return format_is_slice_u8(t);
    if (verb=='b') return t->kind==FE_TYPE_BOOL;
    if (t->kind==FE_TYPE_INT || t->kind==FE_TYPE_BOOL ||
        t->kind==FE_TYPE_CHAR) return 1;
    return format_is_slice_u8(t) ||
        (t->kind==FE_TYPE_ENUM && t->is_error);
}

static void check_format_call(FeCheckerState *s, FeNode *n)
{
    const char *fmt;
    FeNode *fmt_node;
    FeNode *arg;
    FeNode *x;
    FeType *t;
    unsigned long i,j;
    unsigned count=0;
    unsigned argc=0;
    unsigned offset=0;
    int verb;
    int bad=0;
    if (strcmp(n->text,"@fprint")==0) offset=1;
    fmt_node=n->children;
    if (offset) {
        if (!fmt_node) { err(s->c,n->loc,"@fprint requires a writer"); return; }
        t=check_expr(s,fmt_node);
        if (!format_is_writer_type(t))
            err(s->c,fmt_node->loc,"@fprint requires io.Writer");
        fmt_node=fmt_node->next;
    }
    if (strcmp(n->text,"@sprint")==0) {
        if (!fmt_node) { err(s->c,n->loc,"@sprint requires a buffer"); return; }
        t=check_expr(s,fmt_node);
        if (!format_is_slice_u8(t) || !t->ref_mut)
            err(s->c,fmt_node->loc,"@sprint requires []mut u8 buffer");
        fmt_node=fmt_node->next;
    }
    fmt=builtin_format(s,fmt_node);
    if (!fmt) { err(s->c,n->loc,"format must be a comptime string"); return; }
    n->aux_text=(char *)fmt;
    arg=fmt_node ? fmt_node->next : 0;
    for (x=arg;x;x=x->next) { check_expr(s,x); ++argc; }
    i=1;
    while (fmt[i] && fmt[i]!='"') {
        if (fmt[i]=='\\') { if (fmt[i+1]) ++i; ++i; continue; }
        if (fmt[i]=='{' && fmt[i+1]=='{') { i+=2; continue; }
        if (fmt[i]=='}' && fmt[i+1]=='}') { i+=2; continue; }
        if (fmt[i]=='{') {
            j=i+1;
            while (fmt[j] && fmt[j]!='}') ++j;
            if (!fmt[j]) { err(s->c,n->loc,"unterminated format placeholder"); bad=1; break; }
            if (j==i+1) verb=' '; else if (j==i+2) verb=(unsigned char)fmt[i+1]; else verb='?';
            if (verb!=' ' && verb!='x' && verb!='c' && verb!='s' && verb!='b') {
                err(s->c,n->loc,"unsupported format verb"); bad=1;
            }
            if (!arg) { err(s->c,n->loc,"format argument count mismatch"); bad=1; }
            else {
                t=arg->sem_type;
                if (verb==' ' && t && t->kind==FE_TYPE_ENUM && t->is_error) verb='s';
                if (!format_arg_ok(t,verb)) { err(s->c,arg->loc,"no fmt writer for argument type"); bad=1; }
                arg=arg->next;
            }
            ++count; i=j+1; continue;
        }
        if (fmt[i]=='}') { err(s->c,n->loc,"unmatched '}' in format"); bad=1; }
        ++i;
    }
    if (count!=argc) { err(s->c,n->loc,"format argument count mismatch"); bad=1; }
    (void)bad;
}

static int is_format_builtin(const char *name)
{
    return name && (strcmp(name,"@print")==0 || strcmp(name,"@fprint")==0 ||
                    strcmp(name,"@sprint")==0);
}

static int lvalue_writable(FeCheckerState *s, FeNode *n)
{
    FeSym *sym;
    FeType *t;
    if (!n) return 0;
    if (n->kind == FE_N_IDENT) {
        sym=find_symbol(s->scope,n->text ? n->text : "");
        return sym ? sym->mutable : 0;
    }
    if (n->kind == FE_N_MEMBER) {
        t=n->a ? n->a->sem_type : 0;
        if (t && t->kind==FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) return t->ref_mut;
        return lvalue_writable(s,n->a);
    }
    if (n->kind == FE_N_INDEX) return lvalue_writable(s,n->a);
    return 0;
}

static int has_field(FeNode *list, const char *name)
{
    FeNode *f;
    for (f=list; f; f=f->next)
        if (f->text && name && strcmp(f->text,name)==0) return 1;
    return 0;
}

static FeType *check_struct_init(FeCheckerState *s, FeNode *n)
{
    FeType *t;
    FeFieldType *field;
    FeNode *f;
    FeType *v;
    FeType *et;
    FeVariantType *variant;
    unsigned i;
    if (n->a && n->a->kind == FE_N_MEMBER) {
        et=check_expr(s,n->a->a);
        variant=et && et->kind==FE_TYPE_ENUM ?
            fe_type_variant(et,n->a->b ? n->a->b->text : "") : 0;
        if (!variant) { err(s->c,n->loc,"invalid enum variant"); return unknown(s->c); }
        if (variant->field_count != 0) {
            for (f=n->children; f; f=f->next) {
                if (f->kind != FE_N_FIELD) continue;
                field=0;
                if (variant->fields) {
                    unsigned i;
                    for(i=0;i<variant->field_count;i++) if(strcmp(variant->fields[i].name,f->text)==0) field=&variant->fields[i];
                }
                if (!field) { err(s->c,f->loc,"invalid enum payload field"); continue; }
                v=check_expr(s,f->a);
                mark_moved(s,f->a,v);
                if (!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"enum payload type mismatch");
            }
        } else if (n->children) err(s->c,n->loc,"empty enum variant cannot have payload");
        n->sem_type=et; return et;
    }
    t=fe_type_intern(&s->c->types,n->text ? n->text : "<unknown>");
    if (!t || t->kind!=FE_TYPE_STRUCT) { err(s->c,n->loc,"unknown struct type"); return unknown(s->c); }
    for(f=n->children;f;f=f->next) if(f->kind==FE_N_FIELD) {
        if(has_field(f->next,f->text)) { err(s->c,f->loc,"duplicate struct field"); }
        field=fe_type_field(t,f->text);
        if(!field) { err(s->c,f->loc,"invalid struct field"); continue; }
        v=check_expr(s,f->a);
        mark_moved(s,f->a,v);
        if(!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"struct field type mismatch");
    }
    for(i=0;i<t->field_count;i++) if(!has_field(n->children,t->fields[i].name)) err(s->c,n->loc,"missing struct field");
    n->sem_type=t; return t;
}

static FeType *check_array_init(FeCheckerState *s, FeNode *n)
{
    FeNode *x; FeType *elem=0; FeType *v; unsigned long count=0;
    for(x=n->children;x;x=x->next) { v=check_expr(s,x); mark_moved(s,x,v); if(!elem) elem=v; else if(!compatible(elem,v,x)&&v->kind!=FE_TYPE_UNKNOWN) err(s->c,x->loc,"array element type mismatch"); ++count; }
    if(!elem) elem=unknown(s->c);
    n->sem_type=fe_type_array(&s->c->types,count,elem); return n->sem_type;
}

static int array_slice_lvalue(FeNode *n)
{
    return n && (n->kind==FE_N_IDENT || n->kind==FE_N_MEMBER ||
                 n->kind==FE_N_INDEX);
}

static FeType *check_index(FeCheckerState *s, FeNode *n)
{
    FeType *base=check_expr(s,n->a); FeType *idx; FeType *elem;
    if(!fe_type_is_indexable(base)) { err(s->c,n->loc,"indexing requires an array or slice"); return unknown(s->c); }
    if(n->b) { idx=check_expr(s,n->b); if(known(idx)&&!fe_type_is_integer(idx)) err(s->c,n->loc,"index must be an integer"); }
    if(n->c || !n->b) {
        if (base->kind==FE_TYPE_ARRAY && !array_slice_lvalue(n->a))
            err(s->c,n->loc,"array slicing requires a stable lvalue");
        if(n->c) {
            idx=check_expr(s,n->c);
            if(known(idx)&&!fe_type_is_integer(idx))
                err(s->c,n->loc,"slice bound must be an integer");
        }
        elem=base->elem;
        n->sem_type=(base->kind==FE_TYPE_SLICE ? base->ref_mut :
                     lvalue_writable(s,n->a)) ?
            fe_type_mut_slice(&s->c->types,elem) :
            fe_type_slice(&s->c->types,elem);
        return n->sem_type;
    }
    n->sem_type=base->elem; return n->sem_type;
}

static FeType *check_identifier(FeCheckerState *s, FeNode *n, int read)
{
    FeSym *sym;
    sym = find_symbol(s->scope, n->text ? n->text : "");
    if (!sym) {
        FeType *named=fe_type_intern(&s->c->types,n->text ? n->text : "");
        if(named->kind==FE_TYPE_STRUCT || named->kind==FE_TYPE_ENUM) { n->sem_type=named; return named; }
        err(s->c, n->loc, "unknown name");
        return unknown(s->c);
    }
    n->cname = sym->cname;
    n->sem_type = sym->type;
    if (sym->moved == 1) err(s->c,n->loc,"use of moved value");
    else if (sym->moved == 2) err(s->c,n->loc,"use of possibly moved value");
    if (read && !sym->initialized && !sym->fn)
        err(s->c, n->loc, "use of uninitialized variable");
    return sym->type;
}

static FeType *check_expr(FeCheckerState *s, FeNode *n)
{
    FeCheck *c = s->c;
    FeType *a;
    FeType *b;
    FeSym *sym;
    FeNode *x;
    FeNode *param;
    FeNode *arg;
    FeType *et;
    FeFieldType *field;
    FeVariantType *variant;
    const char *op;
    if (!n) return unknown(c);
    if (n->kind == FE_N_IDENT)
        return check_identifier(s, n, 1);
    if (n->kind == FE_N_LITERAL) {
        if (!n->text) return unknown(c);
        if (strcmp(n->text, "true") == 0 || strcmp(n->text, "false") == 0)
            a = fe_type_intern(&c->types, "bool");
        else if (n->text[0] == '\'')
            a = fe_type_intern(&c->types, "char");
        else if (n->text[0] == '"')
            a = fe_type_intern(&c->types, "str");
        else
            a = fe_type_intern(&c->types, "i32");
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_STRUCT_INIT) return check_struct_init(s,n);
    if (n->kind == FE_N_ARRAY_INIT) return check_array_init(s,n);
    if (n->kind == FE_N_INDEX) return check_index(s,n);
    if (n->kind == FE_N_MATCH) { check_match(s,n); n->sem_type=unknown(c); return n->sem_type; }
    if (n->kind == FE_N_UNARY) {
        a = check_expr(s, n->a);
        op = n->text ? n->text : "";
        if (strcmp(op, "not") == 0) {
            if (known(a) && a->kind != FE_TYPE_BOOL)
                err(c, n->loc, "'not' requires bool");
            a = fe_type_intern(&c->types, "bool");
        } else if (strcmp(op, "-") == 0) {
            if (known(a) && !fe_type_is_integer(a))
                err(c, n->loc, "unary '-' requires integer");
        } else if (strcmp(op, "try") == 0) {
            if (a && a->kind==FE_TYPE_ERROR_UNION)
                a=a->error_value;
            else {
                err(c,n->loc,"try requires an error result");
                a=unknown(c);
            }
        } else if (strcmp(op,"&")==0 || strcmp(op,"&mut")==0) {
            a=fe_type_ref(&c->types,a,strcmp(op,"&mut")==0);
        }
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_TYPE && n->text && strcmp(n->text, "as") == 0) {
        a = check_expr(s, n->a);
        b = node_type(c, n->b);
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "cast target cannot be void");
        else if (known(a) && known(b) && !explicit_castable(a,b))
            err(c, n->loc, "'as' requires integer or char types");
        n->sem_type = b;
        return b;
    }
    if (n->kind == FE_N_BINARY) {
        a = check_expr(s, n->a);
        b = check_expr(s, n->b);
        op = n->text ? n->text : "";
        if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
            if ((known(a) && a->kind != FE_TYPE_BOOL) ||
                (known(b) && b->kind != FE_TYPE_BOOL))
                err(c, n->loc, "logical operator requires bool operands");
            a = fe_type_intern(&c->types, "bool");
        } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                   strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
            if (known(a) && known(b) && !fe_type_equal(a, b) &&
                !compatible(a, b, n->b) && !compatible(b, a, n->a))
                err(c, n->loc, "comparison operands have different types");
            a = fe_type_intern(&c->types, "bool");
        } else {
            if ((known(a) && !fe_type_is_integer(a)) ||
                (known(b) && !fe_type_is_integer(b)) ||
                (known(a) && known(b) && !fe_type_equal(a, b) &&
                 !compatible(a, b, n->b) && !compatible(b, a, n->a)))
                err(c, n->loc,
                    "arithmetic operands must have the same integer type");
        }
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_CALL) {
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->b && n->a->b->text &&
            strcmp(n->a->b->text,"drop")==0) {
            err(c,n->loc,"drop may only be invoked by scope cleanup");
            return unknown(c);
        }
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
            n->a->a->kind==FE_N_IDENT && n->a->a->text &&
            strcmp(n->a->a->text,"mem")==0 && n->a->b && n->a->b->text) {
            FeNode *arg=n->children;
            if (strcmp(n->a->b->text,"destroy")==0) {
                a=arg ? check_expr(s,arg) : unknown(c);
                if (!arg || arg->next || !a || a->kind!=FE_TYPE_OWNED)
                    err(c,n->loc,"mem.destroy requires exactly one owned pointer");
                else
                    mark_moved(s,arg,a);
                n->sem_type=fe_type_intern(&c->types,"void");
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"create")==0) {
                if (!arg || arg->next)
                    err(c,n->loc,"mem.create requires exactly one value");
                a=arg ? check_expr(s,arg) : unknown(c);
                if(arg) mark_moved(s,arg,a);
                a=fe_type_owned(&c->types,a);
                n->sem_type=fe_type_error_union(&c->types,a);
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"alloc_slice")==0) {
                FeNode *count=arg ? arg->next : 0;
                FeType *item;
                if(!arg || arg->kind!=FE_N_IDENT || !count || count->next)
                    err(c,n->loc,"mem.alloc_slice requires a type and length");
                item=arg && arg->kind==FE_N_IDENT ?
                    fe_type_intern(&c->types,arg->text) : unknown(c);
                b=count ? check_expr(s,count) : unknown(c);
                if(known(b) && !fe_type_is_integer(b))
                    err(c,count->loc,"slice length must be an integer");
                a=fe_type_owned(&c->types,fe_type_slice(&c->types,item));
                n->sem_type=fe_type_error_union(&c->types,a);
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"replace")==0) {
                FeNode *value=arg ? arg->next : 0;
                if(!arg || !value || value->next)
                    err(c,n->loc,"mem.replace requires destination and value");
                a=arg ? check_expr(s,arg) : unknown(c);
                if(!a || a->kind!=FE_TYPE_REF || !a->ref_mut ||
                   !arg->a || !lvalue_writable(s,arg->a))
                    err(c,n->loc,"mem.replace destination must be a mutable place");
                b=value ? check_expr(s,value) : unknown(c);
                if(a && a->kind==FE_TYPE_REF && !compatible(a->elem,b,value))
                    err(c,value->loc,"mem.replace value type mismatch");
                if(value) mark_moved(s,value,b);
                n->sem_type=a && a->kind==FE_TYPE_REF ? a->elem : unknown(c);
                fe_type_require_replace(&c->types,n->sem_type);
                return n->sem_type;
            }
        }
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
            n->a->a->kind==FE_N_IDENT && n->a->a->text &&
            strcmp(n->a->a->text,"io")==0 && n->a->b && n->a->b->text &&
            strcmp(n->a->b->text,"null_writer")==0) {
            FeNode *arg=n->children;
            if (arg) err(c,n->loc,"io.null_writer takes no arguments");
            n->sem_type=fe_type_intern(&c->types,"io.Writer");
            return n->sem_type;
        }
        if (n->text && is_format_builtin(n->text)) {
            check_format_call(s,n);
            if (strcmp(n->text,"@print")==0)
                n->sem_type=fe_type_intern(&c->types,"void");
            else if (strcmp(n->text,"@sprint")==0)
                n->sem_type=fe_type_intern(&c->types,"usize");
            else
                n->sem_type=fe_type_error_union(&c->types,fe_type_intern(&c->types,"void"));
            return n->sem_type;
        }
        if (!n->a && n->text && (strcmp(n->text,"@size_of")==0 || strcmp(n->text,"@align_of")==0)) {
            FeNode *type_arg=n->children;
            FeType *target=type_arg && type_arg->kind==FE_N_IDENT ? fe_type_intern(&c->types,type_arg->text) : unknown(c);
            if(!target || !known(target)) err(c,n->loc,"size/align requires a known type");
            n->sem_type=fe_type_intern(&c->types,"usize"); return n->sem_type;
        }
        if (n->a && n->a->kind == FE_N_MEMBER) {
            FeNode *method;
            FeNode *self_param;
            et=check_expr(s,n->a->a);
            method=et && et->kind==FE_TYPE_STRUCT ?
                find_method(c,et,n->a->b ? n->a->b->text : "") : 0;
            if(method) {
                self_param=method->a ? method->a->children : 0;
                if(!self_param) {
                    err(c,n->loc,"method requires self parameter");
                    return unknown(c);
                }
                a=method_type(c,self_param->a,et);
                if(a->kind==FE_TYPE_REF && a->ref_mut &&
                   !lvalue_writable(s,n->a->a))
                    err(c,n->loc,"mutable method requires a mutable receiver");
                if(a->kind!=FE_TYPE_REF) mark_moved(s,n->a->a,et);
                param=self_param->next;
                arg=n->children;
                while(param && arg) {
                    a=check_expr(s,arg);
                    b=method_type(c,param->a,et);
                    if(!compatible(b,a,arg) && a->kind!=FE_TYPE_UNKNOWN)
                        err(c,arg->loc,"method argument type mismatch");
                    mark_moved(s,arg,a);
                    param=param->next;
                    arg=arg->next;
                }
                if(param || arg) err(c,n->loc,"wrong number of method arguments");
                n->sem_decl=method;
                n->sem_type=method->b ? method_type(c,method->b,et) :
                    fe_type_intern(&c->types,"void");
                return n->sem_type;
            }
            variant=et && et->kind==FE_TYPE_ENUM ?
                fe_type_variant(et,n->a->b ? n->a->b->text : "") : 0;
            arg=n->children;
            if (!variant) { err(c,n->loc,"invalid enum variant constructor"); return unknown(c); }
            if (variant->field_count==1 && arg) {
                FeType *av=check_expr(s,arg);
                if(!compatible(variant->fields[0].type,av,arg)&&av->kind!=FE_TYPE_UNKNOWN) err(c,arg->loc,"enum payload type mismatch");
            } else if (variant->field_count != 0 || arg) err(c,n->loc,"wrong enum payload arity");
            n->sem_type=et; return et;
        }
        if (n->a && n->a->kind == FE_N_IDENT) {
            sym = find_symbol(s->scope, n->a->text ? n->a->text : "");
            if (!sym) {
                err(c, n->loc, "unknown function");
                return unknown(c);
            }
            n->a->cname = sym->cname;
            n->sem_decl = sym->fn;
            if (!sym->fn) {
                err(c, n->loc, "name is not a function");
                return unknown(c);
            }
            param = sym->fn->a ? sym->fn->a->children : 0;
            arg = n->children;
            while (param && arg) {
                a = check_expr(s, arg);
                mark_moved(s,arg,a);
                b = node_type(c, param->a);
                if (!compatible(b, a, arg) &&
                    !(b && a && b->kind==FE_TYPE_SLICE && a->kind==FE_TYPE_SLICE &&
                      !b->ref_mut && a->ref_mut && fe_type_equal(b->elem,a->elem)) &&
                    a->kind != FE_TYPE_UNKNOWN)
                    err(c, arg->loc, "argument type mismatch");
                param = param->next;
                arg = arg->next;
            }
            if (param || arg) err(c, n->loc, "wrong number of arguments");
            a = sym->fn->b ? node_type(c, sym->fn->b) :
                fe_type_intern(&c->types, "void");
            n->sem_type = a;
            return a;
        }
        for (x = n->children; x; x = x->next) check_expr(s, x);
        return unknown(c);
    }
    if (n->kind == FE_N_MEMBER) {
        if (n->a && n->a->kind==FE_N_IDENT && n->a->text &&
            strcmp(n->a->text,"io")==0 && n->b && n->b->text &&
            (strcmp(n->b->text,"stdout")==0 ||
             strcmp(n->b->text,"stderr")==0)) {
            n->sem_type=fe_type_intern(&c->types,"io.Writer");
            return n->sem_type;
        }
        a=check_expr(s,n->a);
        if (a->kind == FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=a->elem;
            return a->elem;
        }
        if(a->kind==FE_TYPE_REF && a->elem &&
           a->elem->kind==FE_TYPE_STRUCT) {
            field=fe_type_field(a->elem,n->b ? n->b->text : "");
            if(!field) { err(c,n->loc,"unknown struct field"); return unknown(c); }
            n->sem_type=field->type;
            return field->type;
        }
        if (a->kind == FE_TYPE_OWNED && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=a->elem;
            return a->elem;
        }
        if(a->kind==FE_TYPE_STRUCT) {
            field=fe_type_field(a,n->b ? n->b->text : "");
            if(!field) { err(c,n->loc,"unknown struct field"); return unknown(c); }
            n->sem_type=field->type; return field->type;
        }
        if(a->kind==FE_TYPE_ENUM) {
            if(!fe_type_variant(a,n->b ? n->b->text : "")) err(c,n->loc,"unknown enum variant");
            n->sem_type=a; return a;
        }
        if((a->kind==FE_TYPE_SLICE || a->kind==FE_TYPE_STR) && n->b &&
           strcmp(n->b->text,"n")==0) {
            n->sem_type=fe_type_intern(&c->types,"usize"); return n->sem_type;
        }
        return unknown(c);
    }
    return unknown(c);
}

static FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read)
{
    FeSym *sym;
    FeType *base;
    FeFieldType *field;
    if (n && n->kind == FE_N_IDENT) {
        sym = find_symbol(s->scope, n->text ? n->text : "");
        if (!sym) {
            err(s->c, n->loc, "unknown name");
            return unknown(s->c);
        }
        if (sym->fn) {
            err(s->c, n->loc, "function is not assignable");
            return unknown(s->c);
        }
        if (!sym->mutable)
            err(s->c, n->loc, "cannot assign to immutable let");
        n->cname = sym->cname;
        n->sem_type = sym->type;
        if (read && !sym->initialized)
            err(s->c, n->loc, "use of uninitialized variable");
        return sym->type;
    }
    if (n && n->kind == FE_N_MEMBER) {
        base=check_expr(s,n->a);
        if (base && base->kind == FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            if (!base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            n->sem_type=base->elem;
            return base->elem;
        }
        if(base && base->kind==FE_TYPE_REF && base->elem &&
           base->elem->kind==FE_TYPE_STRUCT) {
            if(!base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            field=fe_type_field(base->elem,n->b ? n->b->text : "");
            if(!field) { err(s->c,n->loc,"assignment requires a valid struct field"); return unknown(s->c); }
            n->sem_type=field->type;
            return field->type;
        }
        if (base && base->kind == FE_TYPE_OWNED && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=base->elem;
            return base->elem;
        }
        if (!lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        field=base && base->kind==FE_TYPE_STRUCT ? fe_type_field(base,n->b ? n->b->text : "") : 0;
        if(!field) { err(s->c,n->loc,"assignment requires a valid struct field"); return unknown(s->c); }
        n->sem_type=field->type; return field->type;
    }
    if (n && n->kind == FE_N_INDEX) {
        base=check_index(s,n);
        if (n->a && n->a->sem_type &&
            n->a->sem_type->kind == FE_TYPE_SLICE &&
            !n->a->sem_type->ref_mut)
            err(s->c,n->loc,"cannot write through shared slice");
        else if (n->a && n->a->sem_type &&
                 n->a->sem_type->kind != FE_TYPE_SLICE &&
                 !lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        return base;
    }
    if (n) err(s->c, n->loc, "assignment requires a variable");
    return unknown(s->c);
}

static int compound_operator(const char *op)
{
    return op && strcmp(op, "=") != 0;
}

static void check_match(FeCheckerState *s, FeNode *n)
{
    FeType *value;
    FeNode *arm;
    FeVariantType *variant;
    int seen[256];
    int wildcard=0;
    FeFlowSlot base[64], merged[64], current[64];
    unsigned flow_count;
    int have_merged=0;
    unsigned i;
    for(i=0;i<256U;i++) seen[i]=0;
    value=check_expr(s,n->a);
    if(!value || value->kind!=FE_TYPE_ENUM) { err(s->c,n->loc,"match requires an enum value"); return; }
    flow_count=flow_capture(s->scope,base,64);
    for(arm=n->children;arm;arm=arm->next) {
        FeScope *old=s->scope;
        flow_restore(base,flow_count);
        if(arm->text && strcmp(arm->text,"_")==0) wildcard=1;
        else {
            variant=fe_type_variant(value,arm->text);
            if(!variant) { err(s->c,arm->loc,"unknown match variant"); continue; }
            if(variant->tag<256U) {
                if(seen[variant->tag]) err(s->c,arm->loc,"duplicate match variant");
                seen[variant->tag]=1;
            }
            s->scope=scope_new(s,old);
            if(variant->field_count==1 && arm->children) {
                add_symbol(s,s->scope,arm->children->text,variant->fields[0].type,0,0,1,
                           local_cname(s->c,arm->children->text),arm->children);
            } else if(variant->field_count>0) {
                FeNode *b=arm->children;
                for(i=0;i<variant->field_count && b;i++,b=b->next) {
                    FeFieldType *f=&variant->fields[i];
                    add_symbol(s,s->scope,b->text,f->type,0,0,1,
                               local_cname(s->c,b->text),b);
                }
            }
        }
        if(arm->a && arm->a->kind==FE_N_BLOCK) check_stmt(s,arm->a);
        else if(arm->a) check_expr(s,arm->a);
        s->scope=old;
        flow_capture(s->scope,current,flow_count);
        if(!have_merged) {
            for(i=0;i<flow_count;++i) merged[i]=current[i];
            have_merged=1;
        } else {
            for(i=0;i<flow_count;++i) {
                merged[i].moved=merged[i].moved==1 && current[i].moved==1 ? 1 :
                    (merged[i].moved || current[i].moved ? 2 : 0);
                merged[i].initialized=merged[i].initialized && current[i].initialized;
            }
        }
    }
    if(have_merged) flow_restore(merged,flow_count);
    if(!wildcard) for(i=0;i<value->variant_count && i<256U;i++) if(!seen[i]) err(s->c,n->loc,"non-exhaustive match");
}

static void check_for(FeCheckerState *s, FeNode *n)
{
    FeType *start;
    FeType *finish;
    FeType *elem;
    FeType *ref_type;
    FeSym *iter_sym;
    char *index_cname;
    char *item_cname;
    int iter_mut;
    FeScope *old=s->scope;
    if(!n->c) {
        start=check_expr(s,n->a);
        if (!fe_type_is_indexable(start)) {
            err(s->c,n->loc,"for iterable must be an array, slice, or str");
            return;
        }
        elem=start->elem;
        iter_sym=0;
        if (n->a && n->a->kind==FE_N_IDENT)
            iter_sym=find_symbol(s->scope,n->a->text ? n->a->text : "");
        else if (n->a && n->a->kind==FE_N_INDEX && n->a->a &&
                 n->a->a->kind==FE_N_IDENT)
            iter_sym=find_symbol(s->scope,n->a->a->text ? n->a->a->text : "");
        iter_mut=start->kind==FE_TYPE_SLICE ? start->ref_mut :
            (iter_sym && iter_sym->mutable);
        ref_type=fe_type_ref(&s->c->types,elem,iter_mut);
        if (iter_mut) n->flags |= 4U;
        s->scope=scope_new(s,old);
        if (n->aux_text) {
            index_cname=local_cname(s->c,n->text ? n->text : "index");
            item_cname=local_cname(s->c,n->aux_text);
            add_symbol(s,s->scope,n->text,fe_type_intern(&s->c->types,"usize"),0,0,1,
                       index_cname,n);
            add_symbol(s,s->scope,n->aux_text,ref_type,0,iter_mut,1,
                       item_cname,0);
            n->cname=index_cname;
            n->aux_cname=item_cname;
        } else {
            item_cname=local_cname(s->c,n->text ? n->text : "item");
            add_symbol(s,s->scope,n->text,ref_type,0,iter_mut,1,
                       item_cname,n);
            n->cname=item_cname;
        }
        check_stmt(s,n->b);
        s->scope=old;
        return;
    }
    start=check_expr(s,n->a);
    finish=check_expr(s,n->c);
    if(known(start)&&!fe_type_is_integer(start)) err(s->c,n->loc,"range start must be integer");
    if(known(finish)&&!fe_type_is_integer(finish)) err(s->c,n->loc,"range end must be integer");
    s->scope=scope_new(s,old);
    index_cname=local_cname(s->c,n->text ? n->text : "index");
    add_symbol(s,s->scope,n->text,fe_type_intern(&s->c->types,"usize"),0,0,1,
               index_cname,n);
    n->cname=index_cname;
    check_stmt(s,n->b);
    s->scope=old;
}

static void check_type_cycle(FeCheck *c, FeType *t)
{
    unsigned i;
    FeType *next;
    if (!t || t->kind == FE_TYPE_SLICE || t->kind == FE_TYPE_STR ||
        t->kind == FE_TYPE_REF || t->kind == FE_TYPE_OWNED ||
        t->kind == FE_TYPE_INT || t->kind == FE_TYPE_BOOL ||
        t->kind == FE_TYPE_CHAR || t->kind == FE_TYPE_VOID ||
        t->kind == FE_TYPE_UNKNOWN || t->kind == FE_TYPE_ERROR) return;
    if (t->kind == FE_TYPE_ERROR_UNION) {
        check_type_cycle(c,t->error_value);
        return;
    }
    if (t->cycle_state == 1) {
        if (c->ast->root) err(c, c->ast->root->loc, "by-value recursive type");
        return;
    }
    if (t->cycle_state == 2) return;
    t->cycle_state = 1;
    if (t->kind == FE_TYPE_ARRAY) {
        check_type_cycle(c,t->elem);
    } else if (t->kind == FE_TYPE_STRUCT) {
        for (i=0;i<t->field_count;i++) {
            if (!t->fields[i].type && t->fields[i].ast_node)
                t->fields[i].type=fe_type_from_ast(&c->types,t->fields[i].ast_node->a);
            check_type_cycle(c,t->fields[i].type);
        }
    } else if (t->kind == FE_TYPE_ENUM) {
        for (i=0;i<t->variant_count;i++) {
            unsigned j;
            for (j=0;j<t->variants[i].field_count;j++) {
                if (!t->variants[i].fields[j].type && t->variants[i].fields[j].ast_node)
                    t->variants[i].fields[j].type=fe_type_from_ast(&c->types,
                        t->variants[i].fields[j].ast_node->a);
                next=t->variants[i].fields[j].type;
                check_type_cycle(c,next);
            }
        }
    }
    t->cycle_state=2;
}

static void check_type_cycles(FeCheck *c)
{
    FeType *t;
    for (t=c->types.types;t;t=t->next) t->cycle_state=0;
    for (t=c->types.types;t;t=t->next) check_type_cycle(c,t);
}

static void check_stmt(FeCheckerState *s, FeNode *n)
{
    FeCheck *c = s->c;
    FeScope *old;
    FeType *a;
    FeType *b;
    FeSym *sym;
    FeNode *x;
    int initialized;
    if (!n) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        old = s->scope;
        s->scope = scope_new(s, old);
        for (x = n->children; x; x = x->next) check_stmt(s, x);
        s->scope = old;
        break;
    case FE_N_LET:
    case FE_N_CONST:
        a = n->a ? node_type(c, n->a) : unknown(c);
        b = check_expr(s, n->b);
        if (!n->a) a = b;
        if (a->kind == FE_TYPE_VOID)
            err(c, n->loc, "variable cannot have void type");
        if (n->a && !compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "initializer type mismatch");
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "void expression cannot initialize a variable");
        if (n->kind==FE_N_LET && a->kind==FE_TYPE_SLICE && a->ref_mut)
            err(c,n->loc,"let cannot bind a mutable slice");
        mark_moved(s,n->b,b);
        add_symbol(s, s->scope, n->text, a, 0, 0, 1,
                   local_cname(c, n->text ? n->text : "local"), n);
        break;
    case FE_N_VAR:
        a = n->a ? node_type(c, n->a) : unknown(c);
        if (!n->b && !n->a)
            err(c, n->loc, "uninitialized var requires an explicit type");
        b = n->b ? check_expr(s, n->b) : unknown(c);
        if (!n->a && n->b) a = b;
        if (a->kind == FE_TYPE_VOID)
            err(c, n->loc, "variable cannot have void type");
        if (n->b && !compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "initializer type mismatch");
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "void expression cannot initialize a variable");
        mark_moved(s,n->b,b);
        initialized = n->b != 0;
        add_symbol(s, s->scope, n->text, a, 0, 1, initialized,
                   local_cname(c, n->text ? n->text : "local"), n);
        break;
    case FE_N_ASSIGN:
        b = check_expr(s, n->b);
        a = check_lvalue(s, n->a, compound_operator(n->text));
        if (!compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "assignment type mismatch");
        mark_moved(s,n->b,b);
        sym = n->a && n->a->kind == FE_N_IDENT ?
            find_symbol(s->scope, n->a->text) : 0;
        if (sym && sym->mutable) sym->initialized = 1;
        break;
    case FE_N_EXPR_STMT:
        check_expr(s, n->a);
        if (n->a && n->a->kind==FE_N_UNARY && n->a->text &&
            strcmp(n->a->text,"try")==0 &&
            (!s->ret || s->ret->kind!=FE_TYPE_ERROR_UNION))
            err(c,n->loc,"try requires an enclosing error result");
        break;
    case FE_N_DEFER:
        ++s->defer_depth;
        check_stmt(s,n->a);
        --s->defer_depth;
        break;
    case FE_N_IF: {
        FeFlowSlot base[64], left[64], right[64];
        unsigned flow_count;
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "if condition must be bool");
        flow_count=flow_capture(s->scope,base,64);
        check_stmt(s, n->b);
        flow_capture(s->scope,left,flow_count);
        flow_restore(base,flow_count);
        if (n->c) check_stmt(s, n->c);
        if (n->c) flow_capture(s->scope,right,flow_count);
        else {
            unsigned i;
            for (i=0;i<flow_count;++i) right[i]=base[i];
        }
        flow_merge(base,left,right,flow_count);
        break;
    }
    case FE_N_WHILE: {
        FeFlowSlot base[64], body[64], entry2[64];
        unsigned flow_count;
        unsigned i;
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "while condition must be bool");
        flow_count=flow_capture(s->scope,base,64);
        if (s->loop_depth < 255U) ++s->loop_depth;
        check_stmt(s, n->b);
        if (s->loop_depth) --s->loop_depth;
        flow_capture(s->scope,body,flow_count);
        for (i=0;i<flow_count;++i) {
            entry2[i]=base[i];
            if(body[i].moved!=base[i].moved) entry2[i].moved=2;
            if(!body[i].initialized) entry2[i].initialized=0;
        }
        flow_restore(entry2,flow_count);
        if (s->loop_depth < 255U) ++s->loop_depth;
        check_stmt(s,n->b);
        if (s->loop_depth) --s->loop_depth;
        flow_capture(s->scope,body,flow_count);
        for(i=0;i<flow_count;++i) {
            if(body[i].moved) entry2[i].moved=2;
            if(!body[i].initialized) entry2[i].initialized=0;
        }
        flow_restore(entry2,flow_count);
        break;
    }
    case FE_N_FOR:
        if (s->loop_depth < 255U) ++s->loop_depth;
        check_for(s,n);
        if (s->loop_depth) --s->loop_depth;
        break;
    case FE_N_MATCH:
        check_match(s,n);
        break;
    case FE_N_BREAK:
    case FE_N_CONTINUE:
        if (!s->loop_depth) err(c,n->loc,"break or continue outside loop");
        break;
    case FE_N_RETURN:
        b = n->a ? check_expr(s, n->a) : fe_type_intern(&c->types, "void");
        mark_moved(s,n->a,b);
        if (known(b) && b->kind == FE_TYPE_VOID && s->ret->kind != FE_TYPE_VOID)
            err(c, n->loc, "void expression returned from value function");
        else if (known(s->ret) && known(b) && !fe_type_equal(s->ret, b) &&
                 b->kind != FE_TYPE_UNKNOWN &&
                 !compatible(s->ret,b,n->a))
            err(c, n->loc, "return type mismatch");
        break;
    case FE_N_UNSAFE:
        check_stmt(s, n->a);
        break;
    default:
        break;
    }
}

static void check_fn(FeCheck *c, FeNode *fn, FeScope *globals)
{
    FeCheckerState s;
    FeScope *old;
    FeNode *x;
    FeType *t;
    s.c = c;
    s.globals = globals;
    s.scope = scope_new(&s, globals);
    s.ret = fn->b ? node_type(c, fn->b) : fe_type_intern(&c->types, "void");
    s.loop_depth=0;
    s.defer_depth=0;
    fn->sem_type = s.ret;
    for (x = fn->a ? fn->a->children : 0; x; x = x->next) {
        t = node_type(c, x->a);
        if (t->kind == FE_TYPE_VOID)
            err(c, x->loc, "parameter cannot have void type");
        add_symbol(&s, s.scope, x->text, t, 0, 1, 1,
                   local_cname(c, x->text ? x->text : "arg"), x);
    }
    old = s.scope;
    if (fn->c) check_stmt(&s, fn->c);
    s.scope = old;
}

static void check_method(FeCheck *c, FeNode *fn, FeScope *globals,
                         FeType *owner)
{
    FeCheckerState s;
    FeNode *x;
    FeType *t;
    s.c=c;
    s.globals=globals;
    s.scope=scope_new(&s,globals);
    s.ret=fn->b ? method_type(c,fn->b,owner) : fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    fn->sem_type=s.ret;
    for(x=fn->a ? fn->a->children : 0; x; x=x->next) {
        t=method_type(c,x->a,owner);
        x->sem_type=t;
        add_symbol(&s,s.scope,x->text,t,0,1,1,
                   local_cname(c,x->text ? x->text : "arg"),x);
    }
    if(fn->c) check_stmt(&s,fn->c);
}

int fe_check_program(FeCheck *c)
{
    FeCheckerState s;
    FeNode *n;
    FeSym *sym;
    FeType *t;
    FeType *iv;
    s.c = c;
    s.scope = scope_new(&s, 0);
    s.globals = s.scope;
    s.ret = fe_type_intern(&c->types, "void");
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if (n->kind == FE_N_STRUCT)
            fe_type_declare_struct(&c->types, n, (n->flags & 1U) != 0);
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if (n->kind == FE_N_ENUM) fe_type_declare_enum(&c->types, n);
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if (n->kind == FE_N_ERROR_DECL) fe_type_declare_error(&c->types, n);
    check_type_cycles(c);
    fe_type_layout_all(&c->types);
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if(n->kind==FE_N_STRUCT) {
            FeNode *m;
            char method_name[128];
            for(m=n->children; m; m=m->next) if(m->kind==FE_N_FN) {
                sprintf(method_name,"%s_%s",n->text ? n->text : "Type",
                        m->text ? m->text : "method");
                m->cname=unit_cname(c,method_name);
            }
        }
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            t = n->a ? node_type(c, n->a) : unknown(c);
            add_symbol(&s, s.globals, n->text, t, 0,
                       n->kind == FE_N_GLOBAL, n->b != 0,
                       unit_cname(c, n->text ? n->text : "global"), n);
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if (n->kind == FE_N_FN) {
            t = fe_type_intern(&c->types, "<fn>");
            add_symbol(&s, s.globals, n->text, t, n, 0, 1,
                       unit_cname(c, n->text ? n->text : "fn"), n);
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            sym = find_current(s.globals, n->text ? n->text : "");
            if (n->b) {
                iv = check_expr(&s, n->b);
                if (sym && sym->type->kind == FE_TYPE_UNKNOWN) {
                    sym->type = iv;
                    n->sem_type = iv;
                } else if (sym && !compatible(sym->type, iv, n->b) &&
                           iv->kind != FE_TYPE_UNKNOWN)
                    err(c, n->loc, "global initializer type mismatch");
                if (iv->kind == FE_TYPE_VOID)
                    err(c, n->loc, "void expression cannot initialize a global");
            }
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if (n->kind == FE_N_FN) check_fn(c, n, s.globals);
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if(n->kind==FE_N_STRUCT) {
            FeNode *m;
            t=fe_type_intern(&c->types,n->text);
            for(m=n->children; m; m=m->next)
                if(m->kind==FE_N_FN) check_method(c,m,s.globals,t);
        }
    fe_type_layout_all(&c->types);
    return c->diags->errors == 0;
}

FeType *fe_check_expr_type(FeCheck *c, FeNode *n)
{
    FeCheckerState s;
    s.c = c;
    s.scope = scope_new(&s, 0);
    s.globals = s.scope;
    s.ret = fe_type_intern(&c->types, "void");
    s.loop_depth=0;
    s.defer_depth=0;
    return check_expr(&s, n);
}
