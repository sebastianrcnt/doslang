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
};

struct FeScope {
    FeScope *parent;
    FeSym *items;
    unsigned count;
    unsigned capacity;
};

typedef struct FeCheckerState {
    FeCheck *c;
    FeScope *scope;
    FeScope *globals;
    FeType *ret;
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
static void check_match(FeCheckerState *s, FeNode *n);
static void check_stmt(FeCheckerState *s, FeNode *n);

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
        if(!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"struct field type mismatch");
    }
    for(i=0;i<t->field_count;i++) if(!has_field(n->children,t->fields[i].name)) err(s->c,n->loc,"missing struct field");
    n->sem_type=t; return t;
}

static FeType *check_array_init(FeCheckerState *s, FeNode *n)
{
    FeNode *x; FeType *elem=0; FeType *v; unsigned long count=0;
    for(x=n->children;x;x=x->next) { v=check_expr(s,x); if(!elem) elem=v; else if(!compatible(elem,v,x)&&v->kind!=FE_TYPE_UNKNOWN) err(s->c,x->loc,"array element type mismatch"); ++count; }
    if(!elem) elem=unknown(s->c);
    n->sem_type=fe_type_array(&s->c->types,count,elem); return n->sem_type;
}

static FeType *check_index(FeCheckerState *s, FeNode *n)
{
    FeType *base=check_expr(s,n->a); FeType *idx; FeType *elem;
    if(!fe_type_is_indexable(base)) { err(s->c,n->loc,"indexing requires an array or slice"); return unknown(s->c); }
    if(n->b) { idx=check_expr(s,n->b); if(known(idx)&&!fe_type_is_integer(idx)) err(s->c,n->loc,"index must be an integer"); }
    if(n->c || !n->b) { if(n->c) { idx=check_expr(s,n->c); if(known(idx)&&!fe_type_is_integer(idx)) err(s->c,n->loc,"slice bound must be an integer"); } elem=base->elem; n->sem_type=fe_type_slice(&s->c->types,elem); if(base->kind==FE_TYPE_STR) n->flags|=2U; return n->sem_type; }
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
        if (!n->a && n->text && (strcmp(n->text,"@size_of")==0 || strcmp(n->text,"@align_of")==0)) {
            FeNode *type_arg=n->children;
            FeType *target=type_arg && type_arg->kind==FE_N_IDENT ? fe_type_intern(&c->types,type_arg->text) : unknown(c);
            if(!target || !known(target)) err(c,n->loc,"size/align requires a known type");
            n->sem_type=fe_type_intern(&c->types,"usize"); return n->sem_type;
        }
        if (n->a && n->a->kind == FE_N_MEMBER) {
            et=check_expr(s,n->a->a);
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
            if (!sym->fn) {
                err(c, n->loc, "name is not a function");
                return unknown(c);
            }
            param = sym->fn->a ? sym->fn->a->children : 0;
            arg = n->children;
            while (param && arg) {
                a = check_expr(s, arg);
                b = node_type(c, param->a);
                if (!compatible(b, a, arg) && a->kind != FE_TYPE_UNKNOWN)
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
        a=check_expr(s,n->a);
        if (a->kind == FE_TYPE_REF && n->b && n->b->text &&
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
        if (!lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        field=base && base->kind==FE_TYPE_STRUCT ? fe_type_field(base,n->b ? n->b->text : "") : 0;
        if(!field) { err(s->c,n->loc,"assignment requires a valid struct field"); return unknown(s->c); }
        n->sem_type=field->type; return field->type;
    }
    if (n && n->kind == FE_N_INDEX) {
        if (n->a && n->a->sem_type && n->a->sem_type->kind == FE_TYPE_STR) {
            err(s->c, n->loc, "str is immutable");
        }
        if (n->a && n->a->kind == FE_N_INDEX && (n->a->flags & 2U))
            err(s->c, n->loc, "str slice is immutable");
        if (!lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        base=check_index(s,n);
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
    unsigned i;
    for(i=0;i<256U;i++) seen[i]=0;
    value=check_expr(s,n->a);
    if(!value || value->kind!=FE_TYPE_ENUM) { err(s->c,n->loc,"match requires an enum value"); return; }
    for(arm=n->children;arm;arm=arm->next) {
        FeScope *old=s->scope;
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
    }
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
        iter_mut=iter_sym && iter_sym->mutable;
        if (start->kind==FE_TYPE_STR) iter_mut=0;
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
        t->kind == FE_TYPE_REF ||
        t->kind == FE_TYPE_INT || t->kind == FE_TYPE_BOOL ||
        t->kind == FE_TYPE_CHAR || t->kind == FE_TYPE_VOID ||
        t->kind == FE_TYPE_UNKNOWN || t->kind == FE_TYPE_ERROR) return;
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
        initialized = n->b != 0;
        add_symbol(s, s->scope, n->text, a, 0, 1, initialized,
                   local_cname(c, n->text ? n->text : "local"), n);
        break;
    case FE_N_ASSIGN:
        b = check_expr(s, n->b);
        a = check_lvalue(s, n->a, compound_operator(n->text));
        if (!compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "assignment type mismatch");
        sym = n->a && n->a->kind == FE_N_IDENT ?
            find_symbol(s->scope, n->a->text) : 0;
        if (sym && sym->mutable) sym->initialized = 1;
        break;
    case FE_N_EXPR_STMT:
        check_expr(s, n->a);
        break;
    case FE_N_IF:
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "if condition must be bool");
        check_stmt(s, n->b);
        check_stmt(s, n->c);
        break;
    case FE_N_WHILE:
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "while condition must be bool");
        check_stmt(s, n->b);
        break;
    case FE_N_FOR:
        check_for(s,n);
        break;
    case FE_N_MATCH:
        check_match(s,n);
        break;
    case FE_N_RETURN:
        b = n->a ? check_expr(s, n->a) : fe_type_intern(&c->types, "void");
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
    check_type_cycles(c);
    fe_type_layout_all(&c->types);
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
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
    return check_expr(&s, n);
}
