#include "check.h"
#include "m7.h"
#include <stdlib.h>

#define FE_M7_FLOW_CAP 64U
#include "own.h"
#include <string.h>
#include <stdio.h>

typedef struct FeSym FeSym;
/* FeScope is forward declared in check.h. */

struct FeSym {
    const char *name;
    char *cname;
    FeType *type;
    FeNode *fn;
    int mutable;
    int initialized;
    int moved;
    FeNode *decl;
    /* M6 ownership is tracked at the root local/parameter.  A reference
       binding remembers that root so releasing the binding's last use can
       release the root borrow without a separate alias engine. */
    FeOwnState own;
    FeSym *borrow_root;
    int borrow_mut;
    int borrow_defer;
    FeScope *owner;
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
    FeOwnLiveness liveness;
    FeNode *fn_node;
} FeCheckerState;

static FeType *unknown(FeCheck *c)
{
    return fe_type_intern(&c->types, "<unknown>");
}

static void err(FeCheck *c, FeLoc loc, const char *msg)
{
    fe_diag_error(c->diags, loc, msg);
}

/* Only numbers and characters have an order (SPEC 6.2). */
static int ordered_type(const FeType *t)
{
    return t && (t->kind==FE_TYPE_INT || t->kind==FE_TYPE_CHAR);
}

static int known(FeType *t)
{
    return t && t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_ERROR;
}

/* Is this a projection of `self` inside that type's own `drop`? */
static int in_own_drop(FeCheckerState *s, FeNode *n)
{
    FeNode *base;
    if (!s->fn_node || !s->fn_node->text || strcmp(s->fn_node->text,"drop")!=0)
        return 0;
    base = n ? n->a : 0;
    while (base && (base->kind==FE_N_MEMBER || base->kind==FE_N_INDEX))
        base = base->a;
    return base && base->kind==FE_N_IDENT && base->text &&
           strcmp(base->text,"self")==0;
}

static void mark_moved(FeCheckerState *s, FeNode *n, FeType *t)
{
    FeSym *sym=0;
    /* Inside a type's own `drop` the object is going away, so taking a field
       out of it leaves nothing behind that anyone could read. That is the one
       place R7 has nothing to protect. */
    if (n && (n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX) && in_own_drop(s,n))
        return;
    if (n && n->kind==FE_N_IDENT)
        sym=find_symbol(s->scope,n->text ? n->text : "");
    if (s->defer_depth != 0) {
        /* A defer capture keeps the owner live until scope cleanup; its body
           is not an immediate consuming use. */
        fe_own_mark_consumed(s->c->diags,
                             sym ? &sym->moved : 0,
                             sym ? sym->decl : 0,
                             n,t,1);
        return;
    }
    if (sym && t && !fe_own_is_copy_type(t)) {
        if (n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX) {
            fe_diag_error(s->c->diags,n->loc,
                "cannot move a non-Copy value out of a projection; use mem.replace");
            return;
        }
        /* Reaching an identifier already ran FE_OWN_READ over it, and that
           read reported the value as gone if it was. Running the move as well
           reports the same sentence at the same column a second time, so stop
           at the state the read left behind. */
        if (sym->own.move != FE_OWN_AVAILABLE) return;
        if (fe_own_access(s->c->diags,&sym->own,FE_OWN_MOVE,n->loc)) {
            sym->moved=sym->own.move;
            /* Keep the existing emitter contract: ownership-consuming AST
               uses carry this flag, while FeOwnState is the diagnostic
               authority. */
            fe_own_mark_consumed(s->c->diags,&sym->moved,sym->decl,n,t,0);
        }
        return;
    }
    fe_own_mark_consumed(s->c->diags,
                         sym ? &sym->moved : 0,
                         sym ? sym->decl : 0,
                         n,t,s->defer_depth != 0);
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

/* Does passing `arg` to a parameter of type `param` lend it rather than give
   it away? An exclusive borrow handed to a call comes back when the call
   returns, so it is not a move. */
static int call_reborrows(const FeType *param, const FeType *arg)
{
    if (!param || !arg) return 0;
    if (param->kind==FE_TYPE_REF && arg->kind==FE_TYPE_REF &&
        param->ref_mut && arg->ref_mut) return 1;
    if (param->kind==FE_TYPE_SLICE && arg->kind==FE_TYPE_SLICE &&
        param->ref_mut && arg->ref_mut) return 1;
    return 0;
}

static int explicit_castable(FeType *a, FeType *b)
{
    if (!a || !b) return 0;
    /* An enum without a payload is a number with names on it, so reading it
       as one is a widening or narrowing and nothing more. The other direction
       is not allowed: an arbitrary number is not a variant. */
    if (a->kind == FE_TYPE_ENUM && !a->fields &&
        (fe_type_is_integer(b) || b->kind == FE_TYPE_CHAR)) return 1;
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

/* A link-visible name. A unit path has dots in it and a generic instance has
   brackets and commas, none of which an assembler will accept, so everything
   outside the portable identifier set becomes an underscore. */
static char *unit_cname(FeCheck *c, const char *name)
{
    char *u;
    char *p;
    unsigned long n;
    unsigned long i;
    u = c->ast->root && c->ast->root->text ? c->ast->root->text : "unit";
    n = (unsigned long)strlen("fe_") + (unsigned long)strlen(u) +
        (unsigned long)strlen(name ? name : "name") + 2UL;
    p = (char *)fe_arena_alloc(&c->arena, n);
    if (!p) return 0;
    strcpy(p, "fe_");
    strcat(p, u);
    strcat(p, "_");
    strcat(p, name ? name : "name");
    for (i = 0; p[i]; ++i) {
        char ch = p[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_'))
            p[i] = '_';
    }
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
    p = (char *)fe_arena_alloc(&c->arena, n);
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
    scope = (FeScope *)fe_arena_alloc(&s->c->arena, sizeof(FeScope));
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
        items = (FeSym *)fe_arena_alloc(&s->c->arena,
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
    sym->moved = FE_OWN_AVAILABLE;
    sym->decl = decl;
    fe_own_state_init(&sym->own, initialized);
    sym->borrow_root = 0;
    sym->borrow_mut = 0;
    sym->borrow_defer = 0;
    sym->owner = scope;
    if (decl) {
        decl->cname = cname;
        decl->sem_type = type;
    }
    return sym;
}

/* Make `unit` the one being checked. Types intern against its name, cnames
   are built from it, and diagnostics quote its source rather than whichever
   file happened to be parsed last. */
static void enter_unit(FeCheck *c, unsigned index)
{
    FeUnit *u = &c->build->units[index];
    c->unit = u;
    c->ast = &u->ast;
    c->types.unit_name = u->name[0] ? u->name : "unit";
    fe_diags_source(c->diags, u->source, u->size);
}

/* The type bindings in force, saved across a nested instantiation. */
typedef struct FeBindSave {
    FeTypeBind params[FE_TYPE_PARAM_MAX];
    unsigned count;
} FeBindSave;

static FeType *instantiate_type_node(void *owner, const FeNode *node);

void fe_check_init(FeCheck *c, FeBuild *build, FeDiags *diags,
                   unsigned pointer_bits, int no_checks)
{
    unsigned i;
    fe_arena_init(&c->arena, 16384);
    c->build = build;
    c->unit = 0;
    c->ast = build->count ? &build->units[0].ast : 0;
    for (i = 0; i < FE_BUILD_UNIT_MAX; ++i) c->unit_scope[i] = 0;
    c->diags = diags;
    c->pointer_bits = pointer_bits;
    c->local_serial = 0;
    c->no_checks = no_checks;
    fe_types_init(&c->types, &c->arena, pointer_bits);
    c->types.unit_name = "unit";
    c->types.instantiate = instantiate_type_node;
    c->types.instantiate_owner = c;
    c->instances = (FeInstance *)fe_arena_alloc(&c->arena,
        (unsigned long)FE_GENERIC_INSTANCE_MAX * sizeof(FeInstance));
    c->instance_count = 0;
    c->instance_depth = 0;
}

void fe_check_destroy(FeCheck *c)
{
    fe_arena_destroy(&c->arena);
}

static unsigned unit_index(FeCheck *c, const FeUnit *u)
{
    return (unsigned)(u - c->build->units);
}

/* An import introduces a local binding, so `binding.name` reaches into the
   unit it names. A local of the same spelling wins -- shadowing a binding is
   legal and means the local -- so this only answers when the base name is not
   otherwise in scope. */
static FeUnit *binding_unit(FeCheckerState *s, FeNode *base)
{
    if (!base || base->kind!=FE_N_IDENT || !base->text) return 0;
    if (!s->c->build || !s->c->unit) return 0;
    if (find_symbol(s->scope,base->text)) return 0;
    return fe_build_binding(s->c->build,s->c->unit,base->text);
}

/* SPEC 8.2: a declaration is visible outside its unit only with `pub`. */
static int decl_is_public(const FeNode *decl)
{
    return decl && (decl->flags & FE_NODE_PUB)!=0;
}

static FeSym *unit_member(FeCheck *c, FeUnit *u, const char *name)
{
    if (!u || !name) return 0;
    return find_current(c->unit_scope[unit_index(c,u)],name);
}

/* A type another unit declares, or null if it declares no such type. Interning
   is keyed on the declaring unit, so this cannot collide with a same-named
   type here. */
static FeType *unit_type(FeCheck *c, FeUnit *u, const char *name)
{
    FeType *t;
    if (!u || !name) return 0;
    for (t=c->types.types;t;t=t->next)
        if (t->unit && strcmp(t->name,name)==0 &&
            strcmp(t->unit,u->name)==0 && t->kind!=FE_TYPE_UNKNOWN) return t;
    return 0;
}

/* The AST declaration of a type another unit declares, for its visibility and
   for its methods. */
static FeNode *unit_type_decl(FeCheck *c, FeUnit *u, const char *name)
{
    FeNode *n;
    (void)c;
    if (!u || !name) return 0;
    for (n=u->ast.root ? u->ast.root->children : 0;n;n=n->next)
        if ((n->kind==FE_N_STRUCT || n->kind==FE_N_ENUM ||
             n->kind==FE_N_ERROR_DECL) && n->text &&
            strcmp(n->text,name)==0) return n;
    return 0;
}

/* Resolve a type written in another unit's source. Names in a signature mean
   what they meant where the signature was written, not where it is called. */
static FeType *node_type_in(FeCheck *c, const char *unit, FeNode *node)
{
    const char *save=c->types.unit_name;
    FeType *t;
    if (unit) c->types.unit_name=unit;
    t=node_type(c,node);
    c->types.unit_name=save;
    return t;
}

static FeType *check_expr(FeCheckerState *s, FeNode *n);

static FeNode *find_method(FeCheck *c, FeType *owner, const char *name)
{
    FeNode *decl;
    FeNode *method;
    if(!owner || !name) return 0;
    if(owner->decl_node) {
        for(method=owner->decl_node->children; method; method=method->next)
            if(method->kind==FE_N_FN && method->text &&
               strcmp(method->text,name)==0) return method;
        return 0;
    }
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
static FeType *check_expr_core(FeCheckerState *s, FeNode *n);
static void check_stmt_core(FeCheckerState *s, FeNode *n);
static FeType *check_lvalue_core(FeCheckerState *s, FeNode *n, int read,
                                 FeType *base_in);
static FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read);
static FeType *check_call(FeCheckerState *s, FeNode *n);
static FeType *check_call_args(FeCheckerState *s, FeNode *n, FeSym *sym,
                               const char *home, unsigned skip);
static int is_error_set_member(FeCheckerState *s, FeNode *n);
static void check_fn(FeCheck *c, FeNode *n, FeScope *globals);
static void check_method(FeCheck *c, FeNode *n, FeScope *globals, FeType *owner);
static char *unit_cname(FeCheck *c, const char *name);
static unsigned unit_index(FeCheck *c, const FeUnit *u);
static FeNode *unit_type_decl(FeCheck *c, FeUnit *u, const char *name);
static FeType *check_generic_call(FeCheckerState *s, FeNode *n, FeSym *sym,
                                  FeUnit *home);
static FeType *type_from_expr(FeCheckerState *s, FeNode *n, int *ok);
static FeUnit *current_unit(FeCheck *c);
static int decl_is_generic(const FeNode *decl);
static void check_generic_params(FeCheck *c, FeNode *decl);
static int comptime_condition(FeCheckerState *s, FeNode *n, int *out);
static FeType *check_static_method_call(FeCheckerState *s, FeNode *n,
                                        FeType *owner, FeNode *method);
static FeNode *type_method(FeType *t, const char *name);
static int method_is_static(const FeNode *method);
static int const_names_type(FeCheckerState *s, FeNode *n);
static void push_instance_bindings(FeCheck *c, FeBindSave *save, FeType *t);
static void pop_bindings(FeCheck *c, const FeBindSave *save);
static void bind_self(FeCheck *c, FeType *owner);
static void instance_key(char *out, const char *unit, const char *name,
                         FeType **args, unsigned count);
static int instance_record(FeCheck *c, const char *key, FeLoc loc,
                           FeNode *decl, FeUnit *home, FeType *owner);
static const char *instance_cname(FeCheck *c, const char *key);
static int instance_descend(FeCheck *c, FeLoc loc);
static void instantiate_body(FeCheck *c, FeUnit *home, FeNode *decl,
                             FeType *owner, FeBindSave *bindings, FeLoc site);
static void check_instance_method(FeCheckerState *s, FeType *owner,
                                  FeNode *method, FeLoc site, FeNode *call);

typedef struct FeFlowSlot {
    FeSym *sym;
    int moved;
    int initialized;
    int own_move;
    int own_initialized;
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
            slots[count].own_move=p->items[i].own.move;
            slots[count].own_initialized=p->items[i].own.initialized;
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
        slots[i].sym->own.move=slots[i].own_move;
        slots[i].sym->own.initialized=slots[i].own_initialized;
    }
}

static void flow_merge(FeFlowSlot *base, FeFlowSlot *left, FeFlowSlot *right,
                       unsigned count)
{
    unsigned i;
    for (i=0; i<count; ++i) {
        base[i].sym->moved=fe_own_merge_move(left[i].moved,right[i].moved);
        base[i].sym->initialized=left[i].initialized && right[i].initialized;
        base[i].sym->own.move=fe_own_merge_move(left[i].own_move,right[i].own_move);
        base[i].sym->own.initialized=left[i].own_initialized && right[i].own_initialized;
    }
}

static FeSym *own_root_symbol(FeCheckerState *s, FeNode *expr)
{
    FeOwnPlace place;
    if (!fe_own_place_from_expr(expr,&place)) return 0;
    return find_symbol(s->scope,place.root->text ? place.root->text : "");
}

static int own_is_global(FeCheckerState *s, FeSym *sym)
{
    FeScope *p;
    if (!s || !sym) return 0;
    for (p=s->globals; p; p=p->parent) {
        unsigned i;
        for (i=0;i<p->count;++i) if (&p->items[i]==sym) return 1;
    }
    return 0;
}

static void own_borrow_expr(FeCheckerState *s, FeNode *expr, int mutable)
{
    FeSym *root=own_root_symbol(s,expr);
    if (!root) return;
    if (mutable && root->type && root->type->kind==FE_TYPE_REF &&
        !root->type->ref_mut) {
        err(s->c,expr->loc,"cannot create mutable borrow from a shared reference");
        return;
    }
    if (own_is_global(s,root) &&
        !(root->decl && root->decl->kind==FE_N_GLOBAL &&
          (root->decl->flags & 2U) && !mutable)) {
        err(s->c,expr->loc,"cannot borrow a mutable global");
        return;
    }
    fe_own_access(s->c->diags,&root->own,
                  mutable ? FE_OWN_BORROW_MUT : FE_OWN_BORROW_SHARED,
                  expr->loc);
}

static void own_release_temporary_borrow(FeCheckerState *s, FeNode *expr)
{
    FeSym *root;
    if (!expr || expr->kind!=FE_N_UNARY || !expr->text) return;
    if (strcmp(expr->text,"&")!=0 && strcmp(expr->text,"&mut")!=0) return;
    root=own_root_symbol(s,expr->a);
    if (!root) return;
    if (strcmp(expr->text,"&mut")==0) fe_own_release_exclusive(&root->own);
    else fe_own_release_shared(&root->own);
}

/* Return-reference provenance is represented at call sites by retaining a
   borrow of the unique reference-derived argument (or method receiver). */
static FeSym *own_derived_call_root(FeCheckerState *s, FeNode *call)
{
    FeNode *param;
    FeNode *arg;
    FeNode *source=0;
    unsigned refs=0;
    if (!call || call->kind!=FE_N_CALL || !call->sem_type ||
        !fe_own_is_reference_like(call->sem_type)) return 0;
    if (call->a && call->a->kind==FE_N_MEMBER && call->sem_decl) {
        param=call->sem_decl->a ? call->sem_decl->a->children : 0;
        if (param && param->text && strcmp(param->text,"self")==0)
            return own_root_symbol(s,call->a->a);
    }
    if (!call->sem_decl) return 0;
    param=call->sem_decl->a ? call->sem_decl->a->children : 0;
    arg=call->children;
    while (param && arg) {
        FeType *t=node_type(s->c,param->a);
        if (fe_own_is_reference_like(t)) { ++refs; source=arg; }
        param=param->next;
        arg=arg->next;
    }
    return refs==1 ? own_root_symbol(s,source) : 0;
}

static void own_bind_derived_call(FeCheckerState *s, FeSym *binding,
                                  FeNode *value)
{
    FeSym *root;
    if (!binding || !value || value->kind!=FE_N_CALL) return;
    root=own_derived_call_root(s,value);
    if (!root) return; /* Static provenance. */
    if (root->borrow_root) root=root->borrow_root;
    if (value->sem_type->kind==FE_TYPE_REF && value->sem_type->ref_mut)
        fe_own_access(s->c->diags,&root->own,FE_OWN_BORROW_MUT,value->loc);
    else
        fe_own_access(s->c->diags,&root->own,FE_OWN_BORROW_SHARED,value->loc);
    binding->borrow_root=root;
    binding->borrow_mut=value->sem_type->kind==FE_TYPE_REF && value->sem_type->ref_mut;
}

static int own_stmt_uses(FeNode *node, const char *name)
{
    FeNode *x;
    if (!node || !name) return 0;
    if (node->kind==FE_N_IDENT && node->text && strcmp(node->text,name)==0)
        return 1;
    if (own_stmt_uses(node->a,name) || own_stmt_uses(node->b,name) ||
        own_stmt_uses(node->c,name)) return 1;
    for (x=node->children;x;x=x->next)
        if (own_stmt_uses(x,name)) return 1;
    return 0;
}

static int own_defer_uses(FeNode *node, const char *name)
{
    FeNode *x;
    if (!node) return 0;
    if (node->kind==FE_N_DEFER && own_stmt_uses(node->a,name)) return 1;
    if (own_defer_uses(node->a,name) || own_defer_uses(node->b,name) ||
        own_defer_uses(node->c,name)) return 1;
    for (x=node->children;x;x=x->next)
        if (own_defer_uses(x,name)) return 1;
    return 0;
}

static int own_contains_node(FeNode *node, FeNode *needle)
{
    FeNode *x;
    if (!node || !needle) return 0;
    if (node==needle) return 1;
    if (own_contains_node(node->a,needle) ||
        own_contains_node(node->b,needle) ||
        own_contains_node(node->c,needle)) return 1;
    for (x=node->children;x;x=x->next)
        if (own_contains_node(x,needle)) return 1;
    return 0;
}

static void own_release_after_stmt(FeCheckerState *s, FeScope *scope,
                                   FeNode *stmt, int scope_end)
{
    unsigned i;
    FeScope *p;
    const FeOwnLastUse *last;
    for (p=scope;p;p=scope_end ? 0 : p->parent) for (i=0;i<p->count;++i) {
        FeSym *ref=&p->items[i];
        if (!ref->borrow_root) continue;
        last=fe_own_last_use(&s->liveness,
            ref->decl && ref->decl->text ? ref->decl->text : ref->name);
        if (!scope_end && (ref->borrow_defer || !last || last->defer_extended ||
            !own_contains_node(stmt,last->last_node))) continue;
        if (ref->borrow_mut) fe_own_release_exclusive(&ref->borrow_root->own);
        else fe_own_release_shared(&ref->borrow_root->own);
        ref->borrow_root=0;
    }
}

/* Full borrow snapshots live in the AST arena, rather than on the 16-bit
   compiler stack.  The compact FeFlowSlot arrays retain the pre-M6 move and
   initialization flow handling. */
static FeOwnState *flow_own_new(FeCheckerState *s, unsigned count)
{
    if (!s || !count) return 0;
    return (FeOwnState *)fe_arena_alloc(&s->c->arena,
                                        count*sizeof(FeOwnState));
}

static void flow_own_capture(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) states[i]=slots[i].sym->own;
}

static void flow_own_restore(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) slots[i].sym->own=states[i];
}

static void flow_own_merge(FeFlowSlot *slots, FeOwnState *left,
                           FeOwnState *right, unsigned count)
{
    unsigned i;
    if (!left || !right) return;
    for (i=0;i<count;++i)
        slots[i].sym->own=fe_own_merge_state(left[i],right[i]);
}

typedef struct FeFlowBorrow {
    FeSym *root;
    int mutable;
} FeFlowBorrow;

static FeFlowBorrow *flow_borrow_new(FeCheckerState *s, unsigned count)
{
    if (!s || !count) return 0;
    return (FeFlowBorrow *)fe_arena_alloc(&s->c->arena,
                                          count*sizeof(FeFlowBorrow));
}

static void flow_borrow_capture(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) {
        states[i].root=slots[i].sym->borrow_root;
        states[i].mutable=slots[i].sym->borrow_mut;
    }
}

static void flow_borrow_restore(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) {
        slots[i].sym->borrow_root=states[i].root;
        slots[i].sym->borrow_mut=states[i].mutable;
    }
}

static void flow_borrow_merge(FeFlowSlot *slots, FeFlowBorrow *left,
                              FeFlowBorrow *right, unsigned count)
{
    unsigned i;
    if (!left || !right) return;
    for (i=0;i<count;++i) {
        slots[i].sym->borrow_root=left[i].root ? left[i].root : right[i].root;
        slots[i].sym->borrow_mut=left[i].mutable || right[i].mutable;
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
    int counted=0;
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
            if (!arg) {
                err(s->c,n->loc,"format argument count mismatch");
                bad=1; counted=1;
            }
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
    /* Running out of arguments mid-string already said this. Saying it again
       once the whole string has been walked adds nothing. */
    if (count!=argc && !counted) { err(s->c,n->loc,"format argument count mismatch"); bad=1; }
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

/* A field of a type declared elsewhere is reachable only with `pub`. Inside
   the declaring unit every field is reachable, `pub` or not. */
static int field_is_visible(FeCheckerState *s, const FeType *t,
                            const FeFieldType *field)
{
    if (!t || !t->unit) return 1;
    if (s->c->types.unit_name &&
        strcmp(t->unit,s->c->types.unit_name)==0) return 1;
    return field && field->ast_node &&
           (field->ast_node->flags & FE_NODE_PUB)!=0;
}

/* The field list of a struct literal, once the type is known. Reached from
   both `Type{...}` and `binding.Type{...}`. */
static FeType *check_struct_fields(FeCheckerState *s, FeNode *n, FeType *t)
{
    FeFieldType *field;
    FeNode *f;
    FeType *v;
    unsigned i;
    for(f=n->children;f;f=f->next) if(f->kind==FE_N_FIELD) {
        if(has_field(f->next,f->text)) { err(s->c,f->loc,"duplicate struct field"); }
        field=fe_type_field(t,f->text);
        if(!field) { err(s->c,f->loc,"invalid struct field"); continue; }
        if(!field_is_visible(s,t,field)) {
            err(s->c,f->loc,"field is private to its unit");
            continue;
        }
        v=check_expr(s,f->a);
        mark_moved(s,f->a,v);
        if(!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"struct field type mismatch");
    }
    for(i=0;i<t->field_count;i++) if(!has_field(n->children,t->fields[i].name)) err(s->c,n->loc,"missing struct field");
    n->sem_type=t; return t;
}

static FeType *check_struct_init(FeCheckerState *s, FeNode *n)
{
    FeType *t;
    FeFieldType *field;
    FeNode *f;
    FeType *v;
    FeType *et;
    FeVariantType *variant;
    if (n->a && n->a->kind == FE_N_MEMBER) {
        FeUnit *home=binding_unit(s,n->a->a);
        if (home) {
            /* `binding.Type{...}` names a type in another unit. */
            const char *want=n->a->b && n->a->b->text ? n->a->b->text : "";
            FeNode *decl=unit_type_decl(s->c,home,want);
            t=unit_type(s->c,home,want);
            if (!t || !decl) { err(s->c,n->a->loc,"unknown name"); return unknown(s->c); }
            if (!decl_is_public(decl)) {
                err(s->c,n->a->loc,"type is private to its unit");
                return unknown(s->c);
            }
            if (t->kind!=FE_TYPE_STRUCT) {
                err(s->c,n->loc,"unknown struct type");
                return unknown(s->c);
            }
            return check_struct_fields(s,n,t);
        }
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
    return check_struct_fields(s,n,t);
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

static FeType *check_identifier(FeCheckerState *s, FeNode *n)
{
    FeSym *sym;
    sym = find_symbol(s->scope, n->text ? n->text : "");
    if (!sym) {
        FeType *named=fe_type_intern(&s->c->types,n->text ? n->text : "");
        if(named->kind==FE_TYPE_STRUCT || named->kind==FE_TYPE_ENUM) { n->sem_type=named; return named; }
        if(named->kind!=FE_TYPE_UNKNOWN) {
            err(s->c, n->loc, "a type is not a value here");
            return unknown(s->c);
        }
        err(s->c, n->loc, "unknown name");
        return unknown(s->c);
    }
    n->cname = sym->cname;
    n->sem_type = sym->type;
    if (!sym->fn) {
        fe_own_access(s->c->diags,&sym->own,FE_OWN_READ,n->loc);
        sym->moved=sym->own.move;
    }
    return sym->type;
}

static FeType *check_expr_core(FeCheckerState *s, FeNode *n)
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
        return check_identifier(s, n);
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
            /* SPEC 6.4: try is only allowed inside a function returning an error
               union.  Checked on the expression rather than on the statement so
               that it also covers `var x = try e;` and `x = try e;`, which the
               statement-level check walked straight past. */
            if (!s->ret || s->ret->kind != FE_TYPE_ERROR_UNION)
                err(c,n->loc,"try requires an enclosing error result");
            if (a && a->kind==FE_TYPE_ERROR_UNION)
                a=a->error_value;
            else {
                err(c,n->loc,"try requires an error result");
                a=unknown(c);
            }
        } else if (strcmp(op,"&")==0 || strcmp(op,"&mut")==0) {
            if (strcmp(op,"&mut")==0 && a && a->kind==FE_TYPE_REF && !a->ref_mut)
                err(c,n->loc,"cannot create mutable borrow from a shared reference");
            own_borrow_expr(s,n->a,strcmp(op,"&mut")==0);
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
            else if (strcmp(op,"==")!=0 && strcmp(op,"!=")!=0 &&
                     ((known(a) && !ordered_type(a)) ||
                      (known(b) && !ordered_type(b))))
                err(c, n->loc, "ordering requires integer or char operands");
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
                /* Freshly allocated storage is owned outright, so it is
                   writable: there is nobody else to disturb. */
                a=fe_type_owned(&c->types,fe_type_mut_slice(&c->types,item));
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
            FeUnit *home=binding_unit(s,n->a->a);
            if (home) {
                const char *want=n->a->b && n->a->b->text ? n->a->b->text : "";
                FeSym *fsym=unit_member(c,home,want);
                if (!fsym) {
                    err(c,n->a->loc,"unknown name");
                    for (x=n->children;x;x=x->next) check_expr(s,x);
                    return unknown(c);
                }
                if (!decl_is_public(fsym->decl)) {
                    err(c,n->a->loc,"name is private to its unit");
                    for (x=n->children;x;x=x->next) check_expr(s,x);
                    return unknown(c);
                }
                if (decl_is_generic(fsym->fn))
                    return check_generic_call(s,n,fsym,home);
                return check_call_args(s,n,fsym,home->name,0);
            }
            {
                int names_type=0;
                FeType *owner_type=type_from_expr(s,n->a->a,&names_type);
                if (names_type && owner_type &&
                    owner_type->kind==FE_TYPE_STRUCT) {
                    FeNode *m=type_method(owner_type,
                                          n->a->b ? n->a->b->text : "");
                    if (!m) { err(c,n->a->loc,"unknown method"); return unknown(c); }
                    if (!method_is_static(m)) {
                        err(c,n->loc,"method requires a receiver");
                        return unknown(c);
                    }
                    return check_static_method_call(s,n,owner_type,m);
                }
            }
            et=check_expr(s,n->a->a);
            /* A method can be reached through a reference or an owner as well
               as through the value itself. */
            if (et && (et->kind==FE_TYPE_REF || et->kind==FE_TYPE_OWNED) &&
                et->elem && et->elem->kind==FE_TYPE_STRUCT &&
                find_method(c,et->elem,n->a->b ? n->a->b->text : ""))
                et=et->elem;
            method=et && et->kind==FE_TYPE_STRUCT ?
                find_method(c,et,n->a->b ? n->a->b->text : "") : 0;
            if(method) {
                FeBindSave msave;
                int bound=0;
                self_param=method->a ? method->a->children : 0;
                if(!self_param) {
                    err(c,n->loc,"method requires self parameter");
                    return unknown(c);
                }
                /* A method of a generic instance reads its signature with that
                   instance's arguments bound. */
                if (et->bind_count) {
                    push_instance_bindings(c,&msave,et);
                    bind_self(c,et);
                    bound=1;
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
                if (bound) {
                    pop_bindings(c,&msave);
                    check_instance_method(s,et,method,n->loc,n);
                }
                return n->sem_type;
            }
            if (et && (et->kind==FE_TYPE_SLICE || et->kind==FE_TYPE_STR) &&
                n->a->b && n->a->b->text &&
                strcmp(n->a->b->text,"trim")==0) {
                if (n->children) err(c,n->loc,"trim takes no arguments");
                n->sem_type=fe_type_slice(&c->types,et->elem);
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
            if (decl_is_generic(sym->fn))
                return check_generic_call(s,n,sym,current_unit(c));
            return check_call_args(s, n, sym, 0, 0);
        }
        for (x = n->children; x; x = x->next) check_expr(s, x);
        return unknown(c);
    }
    if (n->kind == FE_N_MEMBER) {
        if (is_error_set_member(s,n)) {
            n->sem_type=fe_type_intern(&c->types,"core.Error");
            return n->sem_type;
        }
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

/* `base_in` is the already-checked type of a member expression's base. The M7
   lvalue path looks at that base before delegating here, and checking it a
   second time reports any ownership violation on it a second time too. */
static FeType *check_lvalue_core(FeCheckerState *s, FeNode *n, int read,
                                 FeType *base_in)
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
        if (read) {
            fe_own_access(s->c->diags,&sym->own,FE_OWN_READ,n->loc);
            sym->moved=sym->own.move;
        }
        return sym->type;
    }
    if (n && n->kind == FE_N_MEMBER) {
        base=base_in ? base_in : check_expr(s,n->a);
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
                merged[i].moved=fe_own_merge_move(merged[i].moved,current[i].moved);
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

static int own_ast_reference_type(FeNode *type)
{
    if (!type || !type->text) return 0;
    return strcmp(type->text,"&")==0 || strcmp(type->text,"&mut")==0 ||
        (strcmp(type->text,"[")==0 && !type->a) || strcmp(type->text,"str")==0;
}

static int own_ast_pointer_to_reference(FeNode *type)
{
    return type && type->text && strcmp(type->text,"*")==0 &&
        own_ast_reference_type(type->a);
}

static void check_reference_storage(FeCheck *c, FeNode *decl)
{
    FeNode *m;
    if (!decl) return;
    if (decl->kind==FE_N_STRUCT || decl->kind==FE_N_ENUM) {
        for (m=decl->children;m;m=m->next)
            if (m->kind==FE_N_FIELD &&
                (own_ast_reference_type(m->a) || own_ast_pointer_to_reference(m->a)))
                err(c,m->loc,"reference type is not allowed in aggregate storage");
    }
    if ((decl->kind==FE_N_GLOBAL || decl->kind==FE_N_CONST) && decl->a &&
        own_ast_reference_type(decl->a) &&
        !(decl->kind==FE_N_CONST && decl->a->text && strcmp(decl->a->text,"str")==0))
        err(c,decl->loc,"reference type is not allowed in global storage");
    if (decl->kind==FE_N_FN && decl->b && own_ast_pointer_to_reference(decl->b))
        err(c,decl->b->loc,"reference type is not allowed as a pointer target");
    if (decl->kind==FE_N_FN)
        for (m=decl->a ? decl->a->children : 0;m;m=m->next)
            if (own_ast_pointer_to_reference(m->a))
                err(c,m->loc,"reference type is not allowed as a pointer target");
}

static int own_return_from_allowed_root(FeCheckerState *s, FeNode *expr)
{
    FeSym *root;
    FeNode *p;
    unsigned refs=0;
    if (!expr) return 0;
    root=own_root_symbol(s,expr);
    if (!root) return 1; /* Static-producing builtins/methods are checked by
                            their declared R8 interface. */
    if (own_is_global(s,root))
        return root->decl && root->decl->kind==FE_N_GLOBAL &&
            (root->decl->flags & 2U);
    if (!root->decl || root->decl->kind!=FE_N_PARAM) return 0;
    for (p=s->fn_node && s->fn_node->a ? s->fn_node->a->children : 0;
         p;p=p->next) {
        FeType *t=p->sem_type ? p->sem_type : node_type(s->c,p->a);
        if (fe_own_is_reference_like(t)) ++refs;
    }
    if (s->fn_node && s->fn_node->text && refs &&
        root->name && strcmp(root->name,"self")==0) return 1;
    return refs==1;
}

static void check_stmt_core(FeCheckerState *s, FeNode *n)
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
        for (x = n->children; x; x = x->next) {
            check_stmt(s,x);
            own_release_after_stmt(s,s->scope,x,0);
        }
        own_release_after_stmt(s,s->scope,n,1);
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
        sym=add_symbol(s, s->scope, n->text, a, 0, 0, 1,
                   local_cname(c, n->text ? n->text : "local"), n);
        if (sym && n->b && n->b->kind==FE_N_UNARY && n->b->text &&
            (strcmp(n->b->text,"&")==0 || strcmp(n->b->text,"&mut")==0)) {
            sym->borrow_root=own_root_symbol(s,n->b->a);
            sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
            sym->borrow_defer=s->defer_depth != 0 ||
                own_defer_uses(s->fn_node ? s->fn_node->c : 0,n->text);
        }
        own_bind_derived_call(s,sym,n->b);
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
        sym=add_symbol(s, s->scope, n->text, a, 0, 1, initialized,
                   local_cname(c, n->text ? n->text : "local"), n);
        if (sym && n->b && n->b->kind==FE_N_UNARY && n->b->text &&
            (strcmp(n->b->text,"&")==0 || strcmp(n->b->text,"&mut")==0)) {
            sym->borrow_root=own_root_symbol(s,n->b->a);
            sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
            sym->borrow_defer=s->defer_depth != 0 ||
                own_defer_uses(s->fn_node ? s->fn_node->c : 0,n->text);
        }
        own_bind_derived_call(s,sym,n->b);
        break;
    case FE_N_ASSIGN:
        b = check_expr(s, n->b);
        a = check_lvalue(s, n->a, compound_operator(n->text));
        if (!compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "assignment type mismatch");
        mark_moved(s,n->b,b);
        sym = n->a && n->a->kind == FE_N_IDENT ?
            find_symbol(s->scope, n->a->text) : 0;
        if (sym && sym->mutable) {
            sym->initialized = 1;
            fe_own_access(s->c->diags,&sym->own,FE_OWN_WRITE,n->a->loc);
            sym->moved=sym->own.move;
            if (n->b && n->b->kind==FE_N_UNARY && n->b->text &&
                (strcmp(n->b->text,"&")==0 || strcmp(n->b->text,"&mut")==0) &&
                fe_own_is_reference_like(sym->type)) {
                FeSym *root=own_root_symbol(s,n->b->a);
                if (root && root->owner!=sym->owner)
                    err(c,n->b->loc,"reference would outlive its source scope");
                else if (root) {
                    if (sym->borrow_root) {
                        if (sym->borrow_mut) fe_own_release_exclusive(&sym->borrow_root->own);
                        else fe_own_release_shared(&sym->borrow_root->own);
                    }
                    sym->borrow_root=root;
                    sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
                }
            }
        }
        break;
    case FE_N_EXPR_STMT:
        /* The enclosing-error-result check lives on the try expression itself,
           so a bare `try e;` needs nothing extra here. */
        check_expr(s, n->a);
        break;
    case FE_N_DEFER:
        ++s->defer_depth;
        check_stmt(s,n->a);
        --s->defer_depth;
        break;
    case FE_N_IF: {
        FeFlowSlot base[64], left[64], right[64];
        FeOwnState *own_base, *own_left, *own_right;
        FeFlowBorrow *borrow_base, *borrow_left, *borrow_right;
        unsigned flow_count;
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "if condition must be bool");
        flow_count=flow_capture(s->scope,base,64);
        own_base=flow_own_new(s,flow_count);
        own_left=flow_own_new(s,flow_count);
        own_right=flow_own_new(s,flow_count);
        borrow_base=flow_borrow_new(s,flow_count);
        borrow_left=flow_borrow_new(s,flow_count);
        borrow_right=flow_borrow_new(s,flow_count);
        flow_own_capture(base,own_base,flow_count);
        flow_borrow_capture(base,borrow_base,flow_count);
        check_stmt(s, n->b);
        flow_capture(s->scope,left,flow_count);
        flow_own_capture(left,own_left,flow_count);
        flow_borrow_capture(left,borrow_left,flow_count);
        flow_restore(base,flow_count);
        flow_own_restore(base,own_base,flow_count);
        flow_borrow_restore(base,borrow_base,flow_count);
        if (n->c) check_stmt(s, n->c);
        if (n->c) {
            flow_capture(s->scope,right,flow_count);
            flow_own_capture(right,own_right,flow_count);
            flow_borrow_capture(right,borrow_right,flow_count);
        }
        else {
            unsigned i;
            for (i=0;i<flow_count;++i) {
                right[i]=base[i];
                if (own_right && own_base) own_right[i]=own_base[i];
                if (borrow_right && borrow_base) borrow_right[i]=borrow_base[i];
            }
        }
        flow_merge(base,left,right,flow_count);
        flow_own_merge(base,own_left,own_right,flow_count);
        flow_borrow_merge(base,borrow_left,borrow_right,flow_count);
        break;
    }
    case FE_N_WHILE: {
        FeFlowSlot base[64], body[64], entry2[64];
        FeOwnState *own_base, *own_body, *own_entry2;
        FeFlowBorrow *borrow_base, *borrow_body, *borrow_entry2;
        unsigned flow_count;
        unsigned i;
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "while condition must be bool");
        flow_count=flow_capture(s->scope,base,64);
        own_base=flow_own_new(s,flow_count);
        own_body=flow_own_new(s,flow_count);
        own_entry2=flow_own_new(s,flow_count);
        borrow_base=flow_borrow_new(s,flow_count);
        borrow_body=flow_borrow_new(s,flow_count);
        borrow_entry2=flow_borrow_new(s,flow_count);
        flow_own_capture(base,own_base,flow_count);
        flow_borrow_capture(base,borrow_base,flow_count);
        if (s->loop_depth < 255U) ++s->loop_depth;
        check_stmt(s, n->b);
        if (s->loop_depth) --s->loop_depth;
        flow_capture(s->scope,body,flow_count);
        flow_own_capture(body,own_body,flow_count);
        flow_borrow_capture(body,borrow_body,flow_count);
        for (i=0;i<flow_count;++i) {
            entry2[i]=base[i];
            entry2[i].moved=fe_own_loop_entry(base[i].moved,body[i].moved);
            if(!body[i].initialized) entry2[i].initialized=0;
            entry2[i].own_move=fe_own_loop_entry(base[i].own_move,body[i].own_move);
            if(!body[i].own_initialized) entry2[i].own_initialized=0;
            if (own_entry2 && own_base && own_body)
                fe_own_loop_merge_state(own_base[i],own_body[i],&own_entry2[i]);
            if (borrow_entry2 && borrow_base && borrow_body)
                borrow_entry2[i]=borrow_base[i].root ? borrow_base[i] : borrow_body[i];
        }
        flow_restore(entry2,flow_count);
        flow_own_restore(entry2,own_entry2,flow_count);
        flow_borrow_restore(entry2,borrow_entry2,flow_count);
        if (s->loop_depth < 255U) ++s->loop_depth;
        check_stmt(s,n->b);
        if (s->loop_depth) --s->loop_depth;
        flow_capture(s->scope,body,flow_count);
        flow_own_capture(body,own_body,flow_count);
        flow_borrow_capture(body,borrow_body,flow_count);
        for(i=0;i<flow_count;++i) {
            entry2[i].moved=fe_own_loop_exit(entry2[i].moved,body[i].moved);
            if(!body[i].initialized) entry2[i].initialized=0;
            entry2[i].own_move=fe_own_loop_exit(entry2[i].own_move,body[i].own_move);
            if(!body[i].own_initialized) entry2[i].own_initialized=0;
            if (own_entry2 && own_body)
                fe_own_loop_merge_state(own_entry2[i],own_body[i],&own_entry2[i]);
            if (borrow_entry2 && borrow_body && !borrow_entry2[i].root)
                borrow_entry2[i]=borrow_body[i];
        }
        flow_restore(entry2,flow_count);
        flow_own_restore(entry2,own_entry2,flow_count);
        flow_borrow_restore(entry2,borrow_entry2,flow_count);
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
        if (s->ret && fe_own_is_reference_like(s->ret) &&
            !own_return_from_allowed_root(s,n->a))
            err(c,n->loc,"reference return must be derived from a parameter or static");
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
    s.fn_node=fn;
    fe_own_liveness_init(&s.liveness,&c->arena);
    fe_own_collect_last_uses(&s.liveness,fn);
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
    s.fn_node=fn;
    fe_own_liveness_init(&s.liveness,&c->arena);
    fe_own_collect_last_uses(&s.liveness,fn);
    fn->sem_type=s.ret;
    for(x=fn->a ? fn->a->children : 0; x; x=x->next) {
        t=method_type(c,x->a,owner);
        x->sem_type=t;
        add_symbol(&s,s.scope,x->text,t,0,1,1,
                   local_cname(c,x->text ? x->text : "arg"),x);
    }
    if(fn->c) check_stmt(&s,fn->c);
}

static int m7_actual_compatible(FeType *want, FeType *got, FeNode *value)
{
    if (fe_type_equal(want,got)) return 1;
    return compatible(want,got,value);
}

static FeType *m7_check_expected(FeCheckerState *s, FeNode *value,
                                 FeType *expected)
{
    FeType *actual;
    FeM7ContextKind context;
    if (!value) return unknown(s->c);
    if (fe_m7_is_null(value)) {
        if (!fe_m7_can_contextual_null(expected)) {
            err(s->c,value->loc,"null requires a contextual optional type");
            value->sem_type=unknown(s->c);
            return value->sem_type;
        }
        value->sem_type=expected;
        value->sem_context=expected;
        return expected;
    }
    actual=check_expr(s,value);
    if (!expected) return actual;
    if (expected->kind==FE_TYPE_OPTIONAL && expected->elem &&
        m7_actual_compatible(expected->elem,actual,value)) {
        value->sem_context=expected;
        return expected;
    }
    if (expected->kind==FE_TYPE_ERROR_UNION) {
        context=fe_m7_error_context(&s->c->types,expected,actual);
        if (context!=FE_M7_CONTEXT_NONE) {
            value->sem_context=expected;
            return expected;
        }
    }
    return actual;
}

static FeType *m7_member_field(FeCheckerState *s, FeNode *n, FeType *base)
{
    FeFieldType *field;
    FeType *owner;
    if (!base) return unknown(s->c);
    if (n->text && strcmp(n->text,".?")==0) {
        if (base->kind!=FE_TYPE_OPTIONAL) {
            err(s->c,n->loc,"optional projection '.?' requires an optional value");
            return unknown(s->c);
        }
        n->sem_type=base->elem;
        return n->sem_type;
    }
    if (base->kind==FE_TYPE_OPTIONAL) {
        err(s->c,n->loc,"optional value must be projected with '.?' first");
        return unknown(s->c);
    }
    if (base->kind==FE_TYPE_REF && n->b && n->b->text &&
        strcmp(n->b->text,"^")==0) {
        n->sem_type=base->elem;
        return n->sem_type;
    }
    if (base->kind==FE_TYPE_OWNED && n->b && n->b->text &&
        strcmp(n->b->text,"^")==0) {
        n->sem_type=base->elem;
        return n->sem_type;
    }
    owner=base;
    if ((base->kind==FE_TYPE_REF || base->kind==FE_TYPE_OWNED) &&
        base->elem && base->elem->kind==FE_TYPE_STRUCT)
        owner=base->elem;
    if (owner && owner->kind==FE_TYPE_STRUCT && n->b && n->b->text) {
        field=fe_type_field(owner,n->b->text);
        if (!field) {
            err(s->c,n->loc,"unknown struct field");
            return unknown(s->c);
        }
        n->sem_type=field->type;
        return field->type;
    }
    if (base->kind==FE_TYPE_ENUM && n->b && n->b->text) {
        if (!fe_type_variant(base,n->b->text))
            err(s->c,n->loc,"unknown enum variant");
        n->sem_type=base;
        return base;
    }
    if ((base->kind==FE_TYPE_SLICE || base->kind==FE_TYPE_STR) &&
        n->b && n->b->text && strcmp(n->b->text,"n")==0) {
        n->sem_type=fe_type_intern(&s->c->types,"usize");
        return n->sem_type;
    }
    n->sem_type=unknown(s->c);
    return n->sem_type;
}

static int m7_place_is_projection(FeNode *n)
{
    return n && (n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX);
}

/* ------------------------------------------------------------------------- *
 * Generics (SPEC 9)
 *
 * A generic declaration is checked once per distinct list of type arguments.
 * Those arguments are bound as types for the length of that check, so a name
 * that is a type parameter simply is its argument -- in the body, in field
 * types and in the signature alike. An instance is identified by its declaring
 * unit, its declaration and the spelling of its arguments, so asking twice
 * asks for the same instance, and a chain of new ones is bounded.
 * ------------------------------------------------------------------------- */

#define FE_GENERIC_DEPTH_MAX 32

static unsigned decl_type_param_count(const FeNode *decl)
{
    FeNode *p;
    unsigned n=0;
    if (!decl) return 0;
    if (decl->kind==FE_N_FN) {
        for (p=decl->a?decl->a->children:0;p;p=p->next)
            if (p->flags & FE_NODE_COMPTIME) ++n;
        return n;
    }
    if (decl->kind==FE_N_STRUCT || decl->kind==FE_N_ENUM)
        for (p=decl->a?decl->a->children:0;p;p=p->next) ++n;
    return n;
}

static FeNode *decl_type_param(const FeNode *decl, unsigned i)
{
    FeNode *p;
    unsigned n=0;
    if (!decl) return 0;
    if (decl->kind==FE_N_FN) {
        for (p=decl->a?decl->a->children:0;p;p=p->next)
            if (p->flags & FE_NODE_COMPTIME) { if (n==i) return p; ++n; }
        return 0;
    }
    for (p=decl->a?decl->a->children:0;p;p=p->next) { if (n==i) return p; ++n; }
    return 0;
}

static int decl_is_generic(const FeNode *decl)
{
    return decl_type_param_count(decl)!=0;
}

/* SPEC 9: v0.1 has comptime type parameters and no other kind. */
static void check_generic_params(FeCheck *c, FeNode *decl)
{
    FeNode *p;
    if (!decl || decl->kind!=FE_N_FN) return;
    for (p=decl->a?decl->a->children:0;p;p=p->next) {
        if (!(p->flags & FE_NODE_COMPTIME)) continue;
        if (!p->a || p->a->kind!=FE_N_TYPE || !p->a->text ||
            strcmp(p->a->text,"type")!=0)
            err(c,p->loc,"a comptime parameter must be a type parameter");
    }
}

static void push_bindings(FeCheck *c, FeBindSave *save, FeNode *decl,
                          FeType **args, unsigned count)
{
    unsigned i;
    save->count=c->types.param_count;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) save->params[i]=c->types.params[i];
    c->types.param_count=0;
    for (i=0;i<count && i<FE_TYPE_PARAM_MAX;++i) {
        FeNode *p=decl_type_param(decl,i);
        c->types.params[i].name=p && p->text ? p->text : "?";
        c->types.params[i].type=args[i];
        ++c->types.param_count;
    }
}

/* Restore the bindings recorded on an instance, so a method sees exactly the
   environment its type was built with. */
static void push_instance_bindings(FeCheck *c, FeBindSave *save, FeType *t)
{
    unsigned i;
    save->count=c->types.param_count;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) save->params[i]=c->types.params[i];
    c->types.param_count=0;
    for (i=0;i<t->bind_count && i<FE_TYPE_PARAM_MAX;++i)
        c->types.params[c->types.param_count++]=t->binds[i];
}

static void bind_self(FeCheck *c, FeType *owner)
{
    if (c->types.param_count>=FE_TYPE_PARAM_MAX) return;
    c->types.params[c->types.param_count].name="Self";
    c->types.params[c->types.param_count].type=owner;
    ++c->types.param_count;
}

static void pop_bindings(FeCheck *c, const FeBindSave *save)
{
    unsigned i;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) c->types.params[i]=save->params[i];
    c->types.param_count=save->count;
}

/* `unit.Name(arg,arg)` -- the canonical identity of one instance.
   Nesting makes the readable spelling grow without bound, and a spelling that
   got cut off would make two different instances look like the same one, so
   past a length the arguments are written as serial numbers instead. Those are
   unique, so identity stays exact even where the spelling stops being
   readable. */
#define FE_GENERIC_NAME_READABLE 200

static void instance_key(char *out, const char *unit, const char *name,
                         FeType **args, unsigned count)
{
    unsigned i;
    unsigned long n=0;
    unsigned long cap=(unsigned long)FE_GENERIC_NAME_READABLE;
    const char *p;
    char number[24];
    int readable=1;
    for (p=unit?unit:"";*p;++p) { if (n<cap) out[n++]=*p; else readable=0; }
    if (n<cap) out[n++]='.'; else readable=0;
    for (p=name?name:"?";*p;++p) { if (n<cap) out[n++]=*p; else readable=0; }
    if (n<cap) out[n++]='('; else readable=0;
    for (i=0;i<count && readable;++i) {
        if (i) { if (n<cap) out[n++]=','; else { readable=0; break; } }
        for (p=args[i] && args[i]->name[0] ? args[i]->name : "?";*p;++p) {
            if (n<cap) out[n++]=*p;
            else { readable=0; break; }
        }
    }
    if (readable && n<cap) out[n++]=')'; else readable=0;
    if (readable) { out[n]='\0'; return; }
    n=0;
    for (p=unit?unit:"";*p && n<cap;++p) out[n++]=*p;
    if (n<cap) out[n++]='.';
    for (p=name?name:"?";*p && n<cap;++p) out[n++]=*p;
    if (n<cap) out[n++]='(';
    for (i=0;i<count;++i) {
        if (i && n<cap) out[n++]=',';
        sprintf(number,"#%u",args[i] ? args[i]->serial : 0U);
        for (p=number;*p && n<cap;++p) out[n++]=*p;
    }
    if (n<cap) out[n++]=')';
    out[n]='\0';
}

/* Already built, or being built right now. Re-asking for a pending instance is
   how a recursive generic terminates, so it must not look like a new one. */
static const char *instance_cname(FeCheck *c, const char *key)
{
    unsigned i;
    for (i=0;i<c->instance_count;++i)
        if (!strcmp(c->instances[i].key,key)) return c->instances[i].cname;
    return 0;
}

static int instance_known(FeCheck *c, const char *key)
{
    unsigned i;
    for (i=0;i<c->instance_count;++i)
        if (strcmp(c->instances[i].key,key)==0) return 1;
    return 0;
}

static int instance_record(FeCheck *c, const char *key, FeLoc loc,
                           FeNode *decl, FeUnit *home, FeType *owner)
{
    FeInstance *inst;
    unsigned i;
    if (instance_known(c,key)) return 0;
    if (c->instance_count>=FE_GENERIC_INSTANCE_MAX) {
        err(c,loc,"too many generic instances");
        return -1;
    }
    inst=&c->instances[c->instance_count];
    strcpy(inst->key,key);
    inst->decl=decl;
    inst->home=home ? home->name : 0;
    inst->owner=owner;
    inst->cname=unit_cname(c,key);
    /* The bindings in force right now are the ones this instance was built
       with, and lowering has to see exactly those again. */
    inst->bind_count=c->types.param_count;
    for (i=0;i<c->types.param_count && i<FE_TYPE_PARAM_MAX;++i)
        inst->binds[i]=c->types.params[i];
    ++c->instance_count;
    return 1;
}

/* One step further down a chain of instantiations. Chains that keep producing
   new instances are the ones that never end, so the limit counts nesting. */
static int instance_descend(FeCheck *c, FeLoc loc)
{
    if (c->instance_depth>=FE_GENERIC_DEPTH_MAX) {
        err(c,loc,"generic instantiation depth exceeded");
        return 0;
    }
    ++c->instance_depth;
    return 1;
}

static FeUnit *current_unit(FeCheck *c)
{
    unsigned u;
    for (u=0;u<c->build->count;++u)
        if (strcmp(c->build->units[u].name,c->types.unit_name)==0)
            return &c->build->units[u];
    return c->unit;
}

/* Build `Box(i32)`: the declaration's fields with the parameters bound, under
   a name that records which arguments made it. */
static FeType *build_struct_instance(FeCheck *c, FeUnit *home, FeNode *decl,
                                     const char *key, FeType **args,
                                     unsigned count)
{
    FeBindSave save;
    FeType *t;
    FeNode *f;
    unsigned fields=0;
    unsigned i=0;
    t=fe_type_intern_unit(&c->types,home->name,key);
    if (!t || t->kind!=FE_TYPE_UNKNOWN) return t;
    t->kind=FE_TYPE_STRUCT;
    t->packed=(decl->flags & FE_NODE_PACKED)!=0;
    t->decl_node=decl;
    t->bind_count=0;
    for (i=0;i<count && i<FE_TYPE_PARAM_MAX;++i) {
        FeNode *p=decl_type_param(decl,i);
        t->binds[t->bind_count].name=p && p->text ? p->text : "?";
        t->binds[t->bind_count].type=args[i];
        ++t->bind_count;
    }
    t->cname=unit_cname(c,key);
    for (f=decl->children;f;f=f->next)
        if (f->kind==FE_N_FN && f->text && strcmp(f->text,"drop")==0)
            t->has_drop=1;
    for (f=decl->children;f;f=f->next) if (f->kind==FE_N_FIELD) ++fields;
    t->field_count=fields;
    if (fields) {
        t->fields=(FeFieldType *)fe_arena_alloc(&c->arena,
                                                fields*sizeof(FeFieldType));
        if (!t->fields) { t->field_count=0; return t; }
        push_instance_bindings(c,&save,t);
        bind_self(c,t);
        i=0;
        for (f=decl->children;f;f=f->next) if (f->kind==FE_N_FIELD) {
            t->fields[i].name=f->text;
            t->fields[i].type=node_type(c,f->a);
            t->fields[i].offset=0;
            t->fields[i].ast_node=f;
            ++i;
        }
        pop_bindings(c,&save);
    }
    fe_type_layout_all(&c->types);
    /* A type that says how to let go of itself needs that method to exist for
       every instance, whether or not anyone calls it by name: scope cleanup
       will. */
    {
        FeNode *release;
        for (release=decl->children;release;release=release->next)
            if (release->kind==FE_N_FN && release->text &&
                !strcmp(release->text,"drop") && release->c) {
                FeCheckerState s;
                memset(&s,0,sizeof s);
                s.c=c;
                s.scope=c->unit_scope[unit_index(c,home)];
                s.globals=s.scope;
                check_instance_method(&s,t,release,decl->loc,0);
                break;
            }
    }
    return t;
}

static FeType *instantiate_struct(FeCheck *c, FeUnit *home, const char *name,
                                  FeType **args, unsigned count, FeLoc loc)
{
    FeNode *decl=unit_type_decl(c,home,name);
    char key[FE_GENERIC_KEY_MAX];
    if (!decl || !decl_is_generic(decl)) {
        err(c,loc,"type does not take generic arguments");
        return unknown(c);
    }
    if (decl->kind!=FE_N_STRUCT) {
        err(c,loc,"only a generic struct can be instantiated");
        return unknown(c);
    }
    if (count!=decl_type_param_count(decl)) {
        err(c,loc,"wrong number of generic arguments");
        return unknown(c);
    }
    instance_key(key,home->name,name,args,count);
    if (instance_record(c,key,loc,decl,home,0)<0) return unknown(c);
    return build_struct_instance(c,home,decl,key,args,count);
}

/* `Name(args...)` written in type position. */
static FeType *instantiate_type_node(void *owner, const FeNode *node)
{
    FeCheck *c=(FeCheck *)owner;
    FeUnit *home=current_unit(c);
    const char *name=node->text;
    FeNode *arg;
    FeType *args[FE_TYPE_PARAM_MAX];
    unsigned count=0;
    FeType *result;
    /* `binding.Name` names a type in another unit. The binding is not itself a
       type, so it has to be peeled off before anything is looked up. */
    if (node->a && node->a->kind==FE_N_IDENT && node->a->text && c->build &&
        c->unit) {
        FeUnit *bound=fe_build_binding(c->build,c->unit,node->text);
        if (bound) { home=bound; name=node->a->text; }
    }
    if (!node->children) {
        FeNode *decl=unit_type_decl(c,home,name ? name : "");
        if (decl && decl_is_generic(decl)) {
            /* A generic declaration is not a type until it has arguments. */
            err(c,node->loc,"generic type requires type arguments");
            return unknown(c);
        }
        if (name!=node->text) {
            FeType *there=unit_type(c,home,name);
            if (there) return there;
        }
        return fe_type_intern(&c->types,name);
    }
    if (!instance_descend(c,node->loc)) return unknown(c);
    for (arg=node->children;arg;arg=arg->next) {
        if (count<FE_TYPE_PARAM_MAX)
            args[count]=fe_type_from_ast(&c->types,arg);
        ++count;
    }
    if (count>FE_TYPE_PARAM_MAX) {
        err(c,node->loc,"wrong number of generic arguments");
        --c->instance_depth;
        return unknown(c);
    }
    result=instantiate_struct(c,home,name ? name : "",args,count,
                              node->loc);
    --c->instance_depth;
    return result;
}

/* A type written where an expression is: `i32`, `Box(i32)`. Only a comptime
   argument position accepts one. */
static FeType *type_from_expr(FeCheckerState *s, FeNode *n, int *ok)
{
    FeCheck *c=s->c;
    FeType *t;
    unsigned i;
    *ok=0;
    if (!n) return unknown(c);
    if (n->kind==FE_N_IDENT && n->text) {
        for (i=0;i<c->types.param_count;++i)
            if (strcmp(c->types.params[i].name,n->text)==0) {
                *ok=1;
                return c->types.params[i].type;
            }
        if (find_symbol(s->scope,n->text)) {
            /* A const alias of a type is that type (SPEC 4.7). */
            FeSym *sym=find_symbol(s->scope,n->text);
            if (sym && sym->decl && sym->decl->kind==FE_N_CONST &&
                sym->decl->b && sym->decl->b->kind==FE_N_IDENT)
                return type_from_expr(s,sym->decl->b,ok);
            return unknown(c);
        }
        t=fe_type_intern(&c->types,n->text);
        if (t && t->kind!=FE_TYPE_UNKNOWN) { *ok=1; return t; }
        return unknown(c);
    }
    if (n->kind==FE_N_CALL && n->a &&
        (n->a->kind==FE_N_IDENT ||
         (n->a->kind==FE_N_MEMBER && n->a->a &&
          n->a->a->kind==FE_N_IDENT && n->a->b && n->a->b->text))) {
        FeType *args[FE_TYPE_PARAM_MAX];
        unsigned count=0;
        FeNode *arg;
        FeType *result;
        FeUnit *home=current_unit(c);
        const char *want;
        /* `Name(args)` here, `binding.Name(args)` when the declaration is in
           another unit. */
        if (n->a->kind==FE_N_MEMBER) {
            FeUnit *bound=binding_unit(s,n->a->a);
            if (!bound) return unknown(c);
            home=bound;
            want=n->a->b->text;
        } else {
            want=n->a->text;
        }
        if (!want || !unit_type_decl(c,home,want)) return unknown(c);
        if (!instance_descend(c,n->loc)) { *ok=1; return unknown(c); }
        for (arg=n->children;arg;arg=arg->next) {
            int inner=0;
            if (count<FE_TYPE_PARAM_MAX)
                args[count]=type_from_expr(s,arg,&inner);
            if (!inner) { --c->instance_depth; return unknown(c); }
            ++count;
        }
        if (count>FE_TYPE_PARAM_MAX) { --c->instance_depth; return unknown(c); }
        result=instantiate_struct(c,home,want,args,count,n->loc);
        --c->instance_depth;
        *ok=1;
        return result;
    }
    return unknown(c);
}

/* A `comptime if` condition. Only the forms SPEC 9 allows: type equality and
   the type predicates. Anything else is not decidable here. */
static int comptime_condition(FeCheckerState *s, FeNode *n, int *out)
{
    FeType *a;
    FeType *b;
    int ok=0;
    int eq;
    if (!n) return 0;
    if (n->kind==FE_N_BINARY && n->text &&
        (strcmp(n->text,"==")==0 || strcmp(n->text,"!=")==0)) {
        a=type_from_expr(s,n->a,&ok);
        if (!ok) return 0;
        b=type_from_expr(s,n->b,&ok);
        if (!ok) return 0;
        eq=fe_type_equal(a,b);
        *out=strcmp(n->text,"==")==0 ? eq : !eq;
        return 1;
    }
    if (n->kind==FE_N_CALL && n->text &&
        (strcmp(n->text,"@is_int")==0 || strcmp(n->text,"@is_ptr")==0)) {
        a=type_from_expr(s,n->children,&ok);
        if (!ok) return 0;
        *out=strcmp(n->text,"@is_int")==0 ? fe_type_is_integer(a) :
             (a && (a->kind==FE_TYPE_OWNED || a->kind==FE_TYPE_REF));
        return 1;
    }
    return 0;
}

/* Check a generic body once, in the unit that declared it and with the
   instance's arguments bound. Errors land on the operation that is wrong; the
   call site gets a note, because the call is context and not the defect. */
static void instantiate_body(FeCheck *c, FeUnit *home, FeNode *decl,
                             FeType *owner, FeBindSave *bindings, FeLoc site)
{
    FeAst *save_ast=c->ast;
    FeUnit *save_unit=c->unit;
    const char *save_name=c->types.unit_name;
    unsigned before=c->diags->errors;
    (void)bindings;
    c->ast=&home->ast;
    c->unit=home;
    c->types.unit_name=home->name;
    fe_diags_source(c->diags,home->source,home->size);
    if (owner) check_method(c,decl,c->unit_scope[unit_index(c,home)],owner);
    else check_fn(c,decl,c->unit_scope[unit_index(c,home)]);
    c->ast=save_ast;
    c->unit=save_unit;
    c->types.unit_name=save_name;
    if (save_unit) fe_diags_source(c->diags,save_unit->source,save_unit->size);
    if (c->diags->errors>before)
        fe_diag_note_src(c->diags,site,"instantiated here");
}

/* A call to a generic function: read the type arguments, check the value
   arguments against the bound signature, then check the body once. */
static FeType *check_generic_call(FeCheckerState *s, FeNode *n, FeSym *sym,
                                  FeUnit *home)
{
    FeCheck *c=s->c;
    FeNode *decl=sym->fn;
    unsigned want=decl_type_param_count(decl);
    FeType *args[FE_TYPE_PARAM_MAX];
    FeNode *arg=n->children;
    unsigned i;
    char key[FE_GENERIC_KEY_MAX];
    FeBindSave save;
    FeType *result;
    int fresh;
    if (want>FE_TYPE_PARAM_MAX) {
        err(c,n->loc,"too many generic parameters");
        return unknown(c);
    }
    for (i=0;i<want;++i) {
        int ok=0;
        if (!arg) {
            err(c,n->loc,"generic call requires explicit type arguments");
            return unknown(c);
        }
        args[i]=type_from_expr(s,arg,&ok);
        if (!ok) {
            err(c,arg->loc,"a comptime type argument must name a type");
            return unknown(c);
        }
        arg=arg->next;
    }
    instance_key(key,home->name,decl->text,args,want);
    push_bindings(c,&save,decl,args,want);
    result=check_call_args(s,n,sym,home->name,want);
    fresh=instance_record(c,key,n->loc,decl,home,0);
    pop_bindings(c,&save);
    /* The call goes to this instance, not to the declaration it came from. */
    if (n->a) n->a->cname=(char *)instance_cname(c,key);
    if (fresh>0) {
        if (!instance_descend(c,n->loc)) return result;
        push_bindings(c,&save,decl,args,want);
        instantiate_body(c,home,decl,0,&save,n->loc);
        pop_bindings(c,&save);
        --c->instance_depth;
    }
    return result;
}

/* `Type.method(...)` where Type is a generic instance and the method takes no
   self parameter. */
/* The unit a name belongs to, by name. */
static FeUnit *unit_named(FeCheck *c, const char *name)
{
    unsigned u;
    if (!name) return 0;
    for (u=0;u<c->build->count;++u)
        if (!strcmp(c->build->units[u].name,name)) return &c->build->units[u];
    return 0;
}

static FeType *check_static_method_call(FeCheckerState *s, FeNode *n,
                                        FeType *owner, FeNode *method)
{
    FeCheck *c=s->c;
    /* A method belongs to the unit that declared its type, not to whichever
       unit happens to be calling it. */
    FeUnit *home=unit_named(c,owner ? owner->unit : 0);
    FeBindSave save;
    FeType *result;
    char key[FE_GENERIC_KEY_MAX];
    FeType *self_args[1];
    int fresh;
    FeSym fake;
    if (!home) home=current_unit(c);
    self_args[0]=owner;
    instance_key(key,home->name,method->text,self_args,1);
    memset(&fake,0,sizeof fake);
    fake.name=method->text;
    fake.cname=method->cname;
    fake.fn=method;
    fake.decl=method;
    push_instance_bindings(c,&save,owner);
    bind_self(c,owner);
    result=check_call_args(s,n,&fake,home->name,0);
    fresh=instance_record(c,key,n->loc,method,home,owner);
    pop_bindings(c,&save);
    if (n->a) n->a->cname=(char *)instance_cname(c,key);
    if (fresh>0) {
        if (!instance_descend(c,n->loc)) return result;
        push_instance_bindings(c,&save,owner);
        bind_self(c,owner);
        instantiate_body(c,home,method,owner,&save,n->loc);
        pop_bindings(c,&save);
        --c->instance_depth;
    }
    return result;
}

/* The body of a method on a generic instance, checked once per instance. */
static void check_instance_method(FeCheckerState *s, FeType *owner,
                                  FeNode *method, FeLoc site, FeNode *call)
{
    FeCheck *c=s->c;
    /* A method belongs to the unit that declared its type, not to whichever
       unit happens to be calling it. */
    FeUnit *home=unit_named(c,owner ? owner->unit : 0);
    FeBindSave save;
    char key[FE_GENERIC_KEY_MAX];
    FeType *self_args[1];
    self_args[0]=owner;
    if (!home) home=current_unit(c);
    instance_key(key,home->name,method->text,self_args,1);
    {
        FeBindSave probe;
        int fresh;
        push_instance_bindings(c,&probe,owner);
        bind_self(c,owner);
        fresh=instance_record(c,key,site,method,home,owner);
        pop_bindings(c,&probe);
        /* The call names this instance's copy of the method. */
        if (call && call->a) call->a->cname=(char *)instance_cname(c,key);
        if (fresh<=0) return;
    }
    if (!instance_descend(c,site)) return;
    push_instance_bindings(c,&save,owner);
    bind_self(c,owner);
    instantiate_body(c,home,method,owner,&save,site);
    pop_bindings(c,&save);
    --c->instance_depth;
}

/* SPEC 4.7: `const Word = i32;` is another spelling of a type, not a value.
   It has no initializer to check and no storage. */
static int const_names_type(FeCheckerState *s, FeNode *n)
{
    FeType *t;
    if (!n->b || n->b->kind!=FE_N_IDENT || !n->b->text) return 0;
    if (n->a) return 0;
    if (find_symbol(s->globals,n->b->text)) return 0;
    t=fe_type_intern(&s->c->types,n->b->text);
    return t && t->kind!=FE_TYPE_UNKNOWN;
}

static FeNode *type_method(FeType *t, const char *name)
{
    FeNode *m;
    if (!t || !t->decl_node || !name) return 0;
    for (m=t->decl_node->children;m;m=m->next)
        if (m->kind==FE_N_FN && m->text && strcmp(m->text,name)==0) return m;
    return 0;
}

static int method_is_static(const FeNode *method)
{
    FeNode *first=method && method->a ? method->a->children : 0;
    return !first || !first->text || strcmp(first->text,"self")!=0;
}

/* A call to a named function. `home` is the unit the signature was written in,
   null when that is the unit being checked: parameter and return types have to
   be read where they were written or a name would mean the caller's type. */
/* `error.Name` is a member of the default error set. That set is open -- names
   are collected across the build and numbered later, not declared -- so any
   name is well formed here and the value's type is core.Error. */
static int is_error_set_member(FeCheckerState *s, FeNode *n)
{
    return n && n->kind==FE_N_MEMBER && n->a && n->a->kind==FE_N_IDENT &&
           n->a->text && strcmp(n->a->text,"error")==0 &&
           n->b && n->b->text && !find_symbol(s->scope,"error");
}

/* `binding.name` used as a value rather than called. */
static FeType *cross_unit_value(FeCheckerState *s, FeNode *n, int *handled)
{
    FeUnit *home=binding_unit(s,n->a);
    FeSym *sym;
    *handled=0;
    if (!home) return 0;
    *handled=1;
    sym=unit_member(s->c,home,n->b && n->b->text ? n->b->text : "");
    if (!sym) { err(s->c,n->loc,"unknown name"); return unknown(s->c); }
    if (!decl_is_public(sym->decl)) {
        err(s->c,n->loc,"name is private to its unit");
        return unknown(s->c);
    }
    n->cname=sym->cname;
    n->sem_decl=sym->decl;
    n->sem_type=sym->type;
    return sym->type;
}

/* `skip` leading parameters and arguments have already been consumed as
   comptime type arguments. */
static FeType *check_call_args(FeCheckerState *s, FeNode *n, FeSym *sym,
                               const char *home, unsigned skip)
{
    FeCheck *c=s->c;
    FeNode *param;
    FeNode *arg;
    FeType *a;
    FeType *b;
    unsigned k;
    if (n->a) n->a->cname = sym->cname;
    n->sem_decl = sym->fn;
    if (!sym->fn) {
        err(c, n->loc, "name is not a function");
        return unknown(c);
    }
    param = sym->fn->a ? sym->fn->a->children : 0;
    arg = n->children;
    for (k=0;k<skip;++k) {
        if (param) param=param->next;
        if (arg) arg=arg->next;
    }
    while (param && arg) {
        a = check_expr(s, arg);
        b = node_type_in(c, home, param->a);
        if (b && a && b->kind==FE_TYPE_REF && !b->ref_mut &&
            a->kind==FE_TYPE_REF && a->ref_mut) {
            FeSym *root=own_root_symbol(s,arg);
            if (root && root->borrow_root) root=root->borrow_root;
            if (root) fe_own_call_shared_view(c->diags,&root->own,arg->loc);
        } else if (b && a && b->kind==FE_TYPE_SLICE && !b->ref_mut &&
                   a->kind==FE_TYPE_SLICE && a->ref_mut) {
            /* Call-only []mut -> [] weakening is a temporary view. */
        } else if (call_reborrows(b, a)) {
            /* Handing an exclusive borrow to a call lends it for the length of
               that call and takes it back after: the caller cannot touch it
               meanwhile, so nothing is aliased. Without this an exclusive
               parameter could be passed onwards exactly once. */
        } else mark_moved(s,arg,a);
        if (!compatible(b, a, arg) &&
            !(b && a && b->kind==FE_TYPE_SLICE && a->kind==FE_TYPE_SLICE &&
              !b->ref_mut && a->ref_mut && fe_type_equal(b->elem,a->elem)) &&
            !(b && a && b->kind==FE_TYPE_REF && a->kind==FE_TYPE_REF &&
              !b->ref_mut && a->ref_mut && fe_type_equal(b->elem,a->elem)) &&
            a->kind != FE_TYPE_UNKNOWN)
            err(c, arg->loc, "argument type mismatch");
        own_release_temporary_borrow(s,arg);
        param = param->next;
        arg = arg->next;
    }
    if (param || arg) err(c, n->loc, "wrong number of arguments");
    a = sym->fn->b ? node_type_in(c, home, sym->fn->b) :
        fe_type_intern(&c->types, "void");
    n->sem_type = a;
    return a;
}

static FeType *check_call(FeCheckerState *s, FeNode *n)
{
    FeCheck *c;
    FeNode *arg;
    FeNode *value;
    FeNode *param;
    FeSym *sym;
    FeType *a;
    FeType *b;
    FeType *expected;
    c=s->c;
    if (n->a && n->a->kind==FE_N_IDENT && n->a->text &&
        strcmp(n->a->text,"Some")==0) {
        err(c,n->loc,"Some is only valid as an optional pattern");
        n->sem_type=unknown(c);
        return n->sem_type;
    }
    if (n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
        n->a->a->kind==FE_N_IDENT && n->a->a->text &&
        strcmp(n->a->a->text,"mem")==0 && n->a->b && n->a->b->text) {
        arg=n->children;
        if (strcmp(n->a->b->text,"replace")==0) {
            value=arg ? arg->next : 0;
            if (!arg || !value || value->next) {
                err(c,n->loc,"mem.replace requires destination and value");
                n->sem_type=unknown(c);
                return n->sem_type;
            }
            a=check_expr(s,arg);
            if (!a || a->kind!=FE_TYPE_REF || !a->ref_mut ||
                !arg->a || !lvalue_writable(s,arg->a))
                err(c,n->loc,"mem.replace destination must be a mutable place");
            expected=a && a->kind==FE_TYPE_REF ? a->elem : 0;
            b=m7_check_expected(s,value,expected);
            if (expected && !fe_type_equal(expected,b) &&
                !m7_actual_compatible(expected,b,value))
                err(c,value->loc,"mem.replace value type mismatch");
            mark_moved(s,value,value->sem_type ? value->sem_type : b);
            n->sem_type=expected ? expected : unknown(c);
            fe_type_require_replace(&c->types,n->sem_type);
            return n->sem_type;
        }
        if (strcmp(n->a->b->text,"destroy")==0) {
            a=arg ? check_expr(s,arg) : unknown(c);
            if (!arg || arg->next || !a || a->kind!=FE_TYPE_OWNED)
                err(c,n->loc,"mem.destroy requires exactly one owned pointer");
            else mark_moved(s,arg,a);
            n->sem_type=fe_type_intern(&c->types,"void");
            return n->sem_type;
        }
        if (strcmp(n->a->b->text,"create")==0 ||
            strcmp(n->a->b->text,"alloc_slice")==0)
            return check_expr_core(s,n);
    }
    if (n->a && n->a->kind==FE_N_IDENT) {
        sym=find_symbol(s->scope,n->a->text ? n->a->text : "");
        if (!sym || !sym->fn) {
            err(c,n->loc,"unknown function");
            n->sem_type=unknown(c);
            return n->sem_type;
        }
        if (decl_is_generic(sym->fn))
            return check_generic_call(s,n,sym,current_unit(c));
        n->a->cname=sym->cname;
        n->sem_decl=sym->fn;
        param=sym->fn->a ? sym->fn->a->children : 0;
        arg=n->children;
        while (param && arg) {
            b=node_type(c,param->a);
            a=m7_check_expected(s,arg,b);
            if (b && a && b->kind==FE_TYPE_REF && !b->ref_mut &&
                a->kind==FE_TYPE_REF && a->ref_mut) {
                FeSym *root;
                root=own_root_symbol(s,arg);
                if (root && root->borrow_root) root=root->borrow_root;
                if (root) fe_own_call_shared_view(c->diags,&root->own,arg->loc);
            } else if (!(b && a && b->kind==FE_TYPE_SLICE &&
                         a->kind==FE_TYPE_SLICE && !b->ref_mut && a->ref_mut) &&
                       !call_reborrows(b, a))
                mark_moved(s,arg,arg->sem_type ? arg->sem_type : a);
            if (!fe_type_equal(b,a) && !m7_actual_compatible(b,a,arg) &&
                !(b && a && b->kind==FE_TYPE_SLICE && a->kind==FE_TYPE_SLICE &&
                  !b->ref_mut && a->ref_mut && fe_type_equal(b->elem,a->elem)) &&
                !(b && a && b->kind==FE_TYPE_REF && a->kind==FE_TYPE_REF &&
                  !b->ref_mut && a->ref_mut && fe_type_equal(b->elem,a->elem)) &&
                a->kind!=FE_TYPE_UNKNOWN)
                err(c,arg->loc,"argument type mismatch");
            own_release_temporary_borrow(s,arg);
            param=param->next;
            arg=arg->next;
        }
        if (param || arg) err(c,n->loc,"wrong number of arguments");
        n->sem_type=sym->fn->b ? node_type(c,sym->fn->b) :
            fe_type_intern(&c->types,"void");
        return n->sem_type;
    }
    return check_expr_core(s,n);
}

static void m7_capture_flow(FeCheckerState *s, FeFlowSlot *slots,
                            FeOwnState **own, FeFlowBorrow **borrow,
                            unsigned *count)
{
    *count=flow_capture(s->scope,slots,FE_M7_FLOW_CAP);
    *own=flow_own_new(s,*count);
    *borrow=flow_borrow_new(s,*count);
    flow_own_capture(slots,*own,*count);
    flow_borrow_capture(slots,*borrow,*count);
}

static void m7_restore_flow(FeFlowSlot *slots, FeOwnState *own,
                            FeFlowBorrow *borrow, unsigned count)
{
    flow_restore(slots,count);
    flow_own_restore(slots,own,count);
    flow_borrow_restore(slots,borrow,count);
}

static void m7_merge_rhs_flow(FeCheckerState *s, FeFlowSlot *base,
                              FeOwnState *own_base,
                              FeFlowBorrow *borrow_base,
                              unsigned count, FeFlowSlot *rhs,
                              FeOwnState *own_rhs,
                              FeFlowBorrow *borrow_rhs)
{
    (void)s;
    flow_merge(base,base,rhs,count);
    flow_own_merge(base,own_base,own_rhs,count);
    flow_borrow_merge(base,borrow_base,borrow_rhs,count);
}

static int m7_stmt_definitely_exits(FeNode *n)
{
    FeNode *last;
    if (!n) return 0;
    if (n->kind==FE_N_RETURN || n->kind==FE_N_BREAK ||
        n->kind==FE_N_CONTINUE) return 1;
    if (n->kind==FE_N_BLOCK) {
        last=n->children;
        if (!last) return 0;
        while (last->next) last=last->next;
        return m7_stmt_definitely_exits(last);
    }
    if (n->kind==FE_N_IF && n->b && n->c)
        return m7_stmt_definitely_exits(n->b) &&
               m7_stmt_definitely_exits(n->c);
    return 0;
}

static FeType *m7_check_lazy(FeCheckerState *s, FeNode *n,
                             FeM7LazyKind kind)
{
    FeType *left_type;
    FeType *payload;
    FeType *right_type;
    FeFlowSlot base[FE_M7_FLOW_CAP];
    FeFlowSlot rhs[FE_M7_FLOW_CAP];
    FeOwnState *own_base;
    FeOwnState *own_rhs;
    FeFlowBorrow *borrow_base;
    FeFlowBorrow *borrow_rhs;
    unsigned count;
    unsigned rhs_count;
    FeScope *old;
    FeType *error_type;
    left_type=check_expr(s,n->a);
    if (kind==FE_M7_LAZY_ORELSE) {
        if (!left_type || left_type->kind!=FE_TYPE_OPTIONAL) {
            err(s->c,n->loc,"orelse requires an optional left operand");
            n->sem_type=unknown(s->c);
            return n->sem_type;
        }
        payload=left_type->elem;
        if (!fe_own_is_copy_type(payload)) {
            if (m7_place_is_projection(n->a))
                err(s->c,n->loc,
                    "non-Copy optional projection requires mem.replace before orelse");
            else
                mark_moved(s,n->a,left_type);
        }
        m7_capture_flow(s,base,&own_base,&borrow_base,&count);
        right_type=m7_check_expected(s,n->b,payload);
        if (!fe_type_equal(payload,right_type) &&
            !m7_actual_compatible(payload,right_type,n->b))
            err(s->c,n->b ? n->b->loc : n->loc,"orelse fallback type mismatch");
        mark_moved(s,n->b,n->b && n->b->sem_type ? n->b->sem_type : right_type);
        rhs_count=flow_capture(s->scope,rhs,FE_M7_FLOW_CAP);
        own_rhs=flow_own_new(s,rhs_count);
        borrow_rhs=flow_borrow_new(s,rhs_count);
        flow_own_capture(rhs,own_rhs,rhs_count);
        flow_borrow_capture(rhs,borrow_rhs,rhs_count);
        if (rhs_count==count)
            m7_merge_rhs_flow(s,base,own_base,borrow_base,count,
                              rhs,own_rhs,borrow_rhs);
        n->sem_type=payload;
        return payload;
    }
    if (!left_type || left_type->kind!=FE_TYPE_ERROR_UNION) {
        err(s->c,n->loc,"catch requires an error result");
        n->sem_type=unknown(s->c);
        return n->sem_type;
    }
    payload=left_type->error_value;
    mark_moved(s,n->a,left_type);
    m7_capture_flow(s,base,&own_base,&borrow_base,&count);
    if (n->c) {
        old=s->scope;
        s->scope=scope_new(s,old);
        error_type=fe_m7_error_type(&s->c->types,left_type);
        if (n->b && n->b->text)
            add_symbol(s,s->scope,n->b->text,error_type,0,0,1,
                       local_cname(s->c,n->b->text),n->b);
        check_stmt(s,n->c);
        s->scope=old;
        if (payload && payload->kind!=FE_TYPE_VOID &&
            !m7_stmt_definitely_exits(n->c))
            err(s->c,n->loc,
                "catch block for a value result must exit instead of falling through");
        if (payload && payload->kind==FE_TYPE_VOID) {
            rhs_count=flow_capture(s->scope,rhs,FE_M7_FLOW_CAP);
            own_rhs=flow_own_new(s,rhs_count);
            borrow_rhs=flow_borrow_new(s,rhs_count);
            flow_own_capture(rhs,own_rhs,rhs_count);
            flow_borrow_capture(rhs,borrow_rhs,rhs_count);
            if (rhs_count==count)
                m7_merge_rhs_flow(s,base,own_base,borrow_base,count,
                                  rhs,own_rhs,borrow_rhs);
        } else {
            m7_restore_flow(base,own_base,borrow_base,count);
        }
        n->sem_type=payload;
        return payload;
    }
    right_type=m7_check_expected(s,n->b,payload);
    if (!fe_type_equal(payload,right_type) &&
        !m7_actual_compatible(payload,right_type,n->b))
        err(s->c,n->b ? n->b->loc : n->loc,"catch fallback type mismatch");
    mark_moved(s,n->b,n->b && n->b->sem_type ? n->b->sem_type : right_type);
    rhs_count=flow_capture(s->scope,rhs,FE_M7_FLOW_CAP);
    own_rhs=flow_own_new(s,rhs_count);
    borrow_rhs=flow_borrow_new(s,rhs_count);
    flow_own_capture(rhs,own_rhs,rhs_count);
    flow_borrow_capture(rhs,borrow_rhs,rhs_count);
    if (rhs_count==count)
        m7_merge_rhs_flow(s,base,own_base,borrow_base,count,
                          rhs,own_rhs,borrow_rhs);
    n->sem_type=payload;
    return payload;
}

static FeType *check_expr(FeCheckerState *s, FeNode *n)
{
    FeType *a;
    FeType *b;
    FeType *ret_error;
    FeType *got_error;
    FeM7LazyKind lazy;
    const char *op;
    if (!n) return unknown(s->c);
    if (fe_m7_is_null(n)) {
        err(s->c,n->loc,"null requires a contextual optional type");
        n->sem_type=unknown(s->c);
        return n->sem_type;
    }
    if (n->kind==FE_N_IDENT)
        return check_identifier(s,n);
    if (n->kind==FE_N_LITERAL)
        return check_expr_core(s,n);
    if (n->kind==FE_N_CALL)
        return check_call(s,n);
    if (n->kind==FE_N_MEMBER) {
        int handled;
        FeType *cross;
        if (is_error_set_member(s,n)) {
            n->sem_type=fe_type_intern(&s->c->types,"core.Error");
            return n->sem_type;
        }
        cross=cross_unit_value(s,n,&handled);
        if (handled) return cross;
        a=check_expr(s,n->a);
        return m7_member_field(s,n,a);
    }
    if (n->kind==FE_N_INDEX)
        return check_index(s,n);
    if (n->kind==FE_N_UNARY) {
        op=n->text ? n->text : "";
        if (strcmp(op,"try")==0) {
            a=check_expr(s,n->a);
            if (!a || a->kind!=FE_TYPE_ERROR_UNION) {
                err(s->c,n->loc,"try requires an error result");
                n->sem_type=unknown(s->c);
                return n->sem_type;
            }
            if (!s->ret || s->ret->kind!=FE_TYPE_ERROR_UNION) {
                err(s->c,n->loc,"try requires an enclosing error result");
            } else {
                ret_error=fe_m7_error_type(&s->c->types,s->ret);
                got_error=fe_m7_error_type(&s->c->types,a);
                if (!ret_error || !got_error || !fe_type_equal(ret_error,got_error))
                    err(s->c,n->loc,
                        "try error type must exactly match the enclosing error result");
            }
            mark_moved(s,n->a,a);
            n->sem_type=a->error_value;
            return n->sem_type;
        }
        if (strcmp(op,"&")==0 || strcmp(op,"&mut")==0) {
            a=check_expr(s,n->a);
            own_borrow_expr(s,n->a,strcmp(op,"&mut")==0);
            n->sem_type=fe_type_ref(&s->c->types,a,strcmp(op,"&mut")==0);
            return n->sem_type;
        }
        return check_expr_core(s,n);
    }
    if (n->kind==FE_N_BINARY) {
        lazy=fe_m7_lazy_kind(n);
        if (lazy!=FE_M7_LAZY_NONE)
            return m7_check_lazy(s,n,lazy);
        op=n->text ? n->text : "";
        if ((strcmp(op,"==")==0 || strcmp(op,"!=")==0) &&
            (fe_m7_is_null(n->a) || fe_m7_is_null(n->b))) {
            FeNode *nonnull;
            FeNode *nullnode;
            nonnull=fe_m7_is_null(n->a) ? n->b : n->a;
            nullnode=fe_m7_is_null(n->a) ? n->a : n->b;
            a=check_expr(s,nonnull);
            if (!a || a->kind!=FE_TYPE_OPTIONAL)
                err(s->c,n->loc,"null comparison requires an optional value");
            else {
                nullnode->sem_type=a;
                nullnode->sem_context=a;
            }
            n->sem_type=fe_type_intern(&s->c->types,"bool");
            return n->sem_type;
        }
        a=check_expr(s,n->a);
        b=check_expr(s,n->b);
        if (strcmp(op,"and")==0 || strcmp(op,"or")==0) {
            if ((known(a) && a->kind!=FE_TYPE_BOOL) ||
                (known(b) && b->kind!=FE_TYPE_BOOL))
                err(s->c,n->loc,"logical operator requires bool operands");
            n->sem_type=fe_type_intern(&s->c->types,"bool");
            return n->sem_type;
        }
        if (strcmp(op,"==")==0 || strcmp(op,"!=")==0 ||
            strcmp(op,"<")==0 || strcmp(op,"<=")==0 ||
            strcmp(op,">")==0 || strcmp(op,">=")==0) {
            if (known(a) && known(b) && !fe_type_equal(a,b) &&
                !m7_actual_compatible(a,b,n->b) &&
                !m7_actual_compatible(b,a,n->a))
                err(s->c,n->loc,"comparison operands have different types");
            /* Only numbers and characters have an order. */
            else if (strcmp(op,"==")!=0 && strcmp(op,"!=")!=0 &&
                     ((known(a) && !ordered_type(a)) ||
                      (known(b) && !ordered_type(b))))
                err(s->c,n->loc,"ordering requires integer or char operands");
            n->sem_type=fe_type_intern(&s->c->types,"bool");
            return n->sem_type;
        }
        if ((known(a) && !fe_type_is_integer(a)) ||
            (known(b) && !fe_type_is_integer(b)) ||
            (known(a) && known(b) && !fe_type_equal(a,b) &&
             !m7_actual_compatible(a,b,n->b) &&
             !m7_actual_compatible(b,a,n->a)))
            err(s->c,n->loc,"arithmetic operands must have the same integer type");
        n->sem_type=a;
        return a;
    }
    if (n->kind==FE_N_TYPE && n->text && strcmp(n->text,"as")==0)
        return check_expr_core(s,n);
    if (n->kind==FE_N_STRUCT_INIT)
        return check_struct_init(s,n);
    if (n->kind==FE_N_ARRAY_INIT)
        return check_array_init(s,n);
    return check_expr_core(s,n);
}

static FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read)
{
    FeType *base=0;
    FeFieldType *field;
    FeType *owner;
    if (!n) return unknown(s->c);
    if (n->kind==FE_N_MEMBER) {
        base=check_expr(s,n->a);
        if (base && base->kind==FE_TYPE_OPTIONAL) {
            err(s->c,n->loc,"optional value must be projected with '.?' first");
            return unknown(s->c);
        }
        if (base && base->kind==FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            if (!base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            n->sem_type=base->elem;
            return base->elem;
        }
        owner=base;
        if ((base->kind==FE_TYPE_REF || base->kind==FE_TYPE_OWNED) &&
            base->elem && base->elem->kind==FE_TYPE_STRUCT)
            owner=base->elem;
        if (owner && owner->kind==FE_TYPE_STRUCT && n->b && n->b->text) {
            if (base->kind==FE_TYPE_REF && !base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            /* Writing a field still needs a writable place. This branch used to
               be reached only by units mentioning M7 syntax, so it never had to
               repeat the check the M6 path does. */
            if (base->kind!=FE_TYPE_REF && base->kind!=FE_TYPE_OWNED &&
                !lvalue_writable(s,n->a))
                err(s->c,n->loc,"cannot assign through immutable value");
            field=fe_type_field(owner,n->b->text);
            if (!field) {
                err(s->c,n->loc,"assignment requires a valid struct field");
                return unknown(s->c);
            }
            n->sem_type=field->type;
            return field->type;
        }
    }
    return check_lvalue_core(s,n,read,n->kind==FE_N_MEMBER ? base : 0);
}

static FeType *m7_pattern_binding_type(FeCheckerState *s, FeType *payload,
                                      FeNode *source, int *borrow_mut)
{
    FeSym *root;
    int mutable;
    *borrow_mut=0;
    if (fe_own_is_copy_type(payload)) return payload;
    root=own_root_symbol(s,source);
    mutable=root && root->mutable;
    *borrow_mut=mutable;
    if (payload->kind==FE_TYPE_OWNED && payload->elem)
        return fe_type_ref(&s->c->types,payload->elem,mutable);
    return fe_type_ref(&s->c->types,payload,mutable);
}

static void m7_check_if_let(FeCheckerState *s, FeNode *n)
{
    FeType *opt;
    FeType *binding_type;
    FeNode *binding;
    FeSym *root;
    FeScope *old;
    FeFlowSlot base[FE_M7_FLOW_CAP];
    FeFlowSlot left[FE_M7_FLOW_CAP];
    FeFlowSlot right[FE_M7_FLOW_CAP];
    FeOwnState *own_base;
    FeOwnState *own_left;
    FeOwnState *own_right;
    FeFlowBorrow *borrow_base;
    FeFlowBorrow *borrow_left;
    FeFlowBorrow *borrow_right;
    unsigned count;
    unsigned i;
    int borrow_mut;
    int is_some;
    opt=check_expr(s,n->a);
    if (!opt || opt->kind!=FE_TYPE_OPTIONAL) {
        err(s->c,n->loc,"if let Some/None requires an optional value");
        return;
    }
    is_some=n->aux_text && strcmp(n->aux_text,"Some")==0;
    if (!is_some && (!n->aux_text || strcmp(n->aux_text,"None")!=0))
        err(s->c,n->loc,"if let optional pattern must be Some or None");
    m7_capture_flow(s,base,&own_base,&borrow_base,&count);
    old=s->scope;
    s->scope=scope_new(s,old);
    root=0;
    borrow_mut=0;
    binding=n->children;
    if (is_some && binding) {
        binding_type=m7_pattern_binding_type(s,opt->elem,n->a,&borrow_mut);
        add_symbol(s,s->scope,binding->text,binding_type,0,borrow_mut,1,
                   local_cname(s->c,binding->text),binding);
        if (!fe_own_is_copy_type(opt->elem)) {
            root=own_root_symbol(s,n->a);
            if (root)
                fe_own_access(s->c->diags,&root->own,
                    borrow_mut ? FE_OWN_BORROW_MUT : FE_OWN_BORROW_SHARED,
                    n->loc);
        }
    }
    check_stmt(s,n->b);
    if (root) {
        if (borrow_mut) fe_own_release_exclusive(&root->own);
        else fe_own_release_shared(&root->own);
    }
    s->scope=old;
    flow_capture(s->scope,left,count);
    own_left=flow_own_new(s,count);
    borrow_left=flow_borrow_new(s,count);
    flow_own_capture(left,own_left,count);
    flow_borrow_capture(left,borrow_left,count);
    m7_restore_flow(base,own_base,borrow_base,count);
    if (n->c) check_stmt(s,n->c);
    if (n->c) {
        flow_capture(s->scope,right,count);
        own_right=flow_own_new(s,count);
        borrow_right=flow_borrow_new(s,count);
        flow_own_capture(right,own_right,count);
        flow_borrow_capture(right,borrow_right,count);
    } else {
        own_right=flow_own_new(s,count);
        borrow_right=flow_borrow_new(s,count);
        for (i=0;i<count;++i) right[i]=base[i];
        if (own_right && own_base)
            for (i=0;i<count;++i) own_right[i]=own_base[i];
        if (borrow_right && borrow_base)
            for (i=0;i<count;++i) borrow_right[i]=borrow_base[i];
    }
    flow_merge(base,left,right,count);
    flow_own_merge(base,own_left,own_right,count);
    flow_borrow_merge(base,borrow_left,borrow_right,count);
}

static void m7_check_optional_match(FeCheckerState *s, FeNode *n,
                                    FeType *opt)
{
    FeNode *arm;
    FeNode *binding;
    FeScope *old;
    FeType *binding_type;
    FeSym *root;
    int borrow_mut;
    int seen_some;
    int seen_none;
    int wildcard;
    FeFlowSlot base[FE_M7_FLOW_CAP];
    FeFlowSlot current[FE_M7_FLOW_CAP];
    FeFlowSlot merged[FE_M7_FLOW_CAP];
    FeOwnState *own_base;
    FeOwnState *own_current;
    FeOwnState *own_merged;
    FeFlowBorrow *borrow_base;
    FeFlowBorrow *borrow_current;
    FeFlowBorrow *borrow_merged;
    unsigned count;
    unsigned i;
    int have;
    seen_some=0;
    seen_none=0;
    wildcard=0;
    have=0;
    m7_capture_flow(s,base,&own_base,&borrow_base,&count);
    own_merged=flow_own_new(s,count);
    borrow_merged=flow_borrow_new(s,count);
    for (arm=n->children;arm;arm=arm->next) {
        m7_restore_flow(base,own_base,borrow_base,count);
        old=s->scope;
        s->scope=scope_new(s,old);
        root=0;
        borrow_mut=0;
        if (arm->text && strcmp(arm->text,"Some")==0) {
            if (seen_some) err(s->c,arm->loc,"duplicate Some match arm");
            seen_some=1;
            binding=arm->children;
            if (binding) {
                binding_type=m7_pattern_binding_type(s,opt->elem,n->a,&borrow_mut);
                add_symbol(s,s->scope,binding->text,binding_type,0,borrow_mut,1,
                           local_cname(s->c,binding->text),binding);
                if (!fe_own_is_copy_type(opt->elem)) {
                    root=own_root_symbol(s,n->a);
                    if (root)
                        fe_own_access(s->c->diags,&root->own,
                            borrow_mut ? FE_OWN_BORROW_MUT : FE_OWN_BORROW_SHARED,
                            arm->loc);
                }
            }
        } else if (arm->text && strcmp(arm->text,"None")==0) {
            if (seen_none) err(s->c,arm->loc,"duplicate None match arm");
            seen_none=1;
        } else if (arm->text && strcmp(arm->text,"_")==0) {
            wildcard=1;
        } else {
            err(s->c,arm->loc,"optional match arm must be Some, None, or _");
        }
        if (arm->a && arm->a->kind==FE_N_BLOCK) check_stmt(s,arm->a);
        else if (arm->a) check_expr(s,arm->a);
        if (root) {
            if (borrow_mut) fe_own_release_exclusive(&root->own);
            else fe_own_release_shared(&root->own);
        }
        s->scope=old;
        flow_capture(s->scope,current,count);
        own_current=flow_own_new(s,count);
        borrow_current=flow_borrow_new(s,count);
        flow_own_capture(current,own_current,count);
        flow_borrow_capture(current,borrow_current,count);
        if (!have) {
            for (i=0;i<count;++i) merged[i]=current[i];
            if (own_merged && own_current)
                for (i=0;i<count;++i) own_merged[i]=own_current[i];
            if (borrow_merged && borrow_current)
                for (i=0;i<count;++i) borrow_merged[i]=borrow_current[i];
            have=1;
        } else {
            flow_merge(merged,merged,current,count);
            if (own_merged && own_current)
                for (i=0;i<count;++i)
                    own_merged[i]=fe_own_merge_state(own_merged[i],own_current[i]);
            if (borrow_merged && borrow_current)
                for (i=0;i<count;++i) {
                    if (!borrow_merged[i].root)
                        borrow_merged[i].root=borrow_current[i].root;
                    borrow_merged[i].mutable=
                        borrow_merged[i].mutable || borrow_current[i].mutable;
                }
        }
    }
    if (!wildcard && (!seen_some || !seen_none))
        err(s->c,n->loc,"non-exhaustive optional match");
    if (have) m7_restore_flow(merged,own_merged,borrow_merged,count);
}

static void m7_check_match_stmt(FeCheckerState *s, FeNode *n)
{
    FeType *value;
    value=check_expr(s,n->a);
    if (value && value->kind==FE_TYPE_OPTIONAL) {
        m7_check_optional_match(s,n,value);
        n->sem_type=unknown(s->c);
        return;
    }
    check_match(s,n);
}

static void m7_check_decl_stmt(FeCheckerState *s, FeNode *n, int mutable)
{
    FeType *expected;
    FeType *actual;
    FeType *stored;
    FeSym *sym;
    int initialized;
    expected=n->a ? node_type(s->c,n->a) : 0;
    if (n->b)
        stored=m7_check_expected(s,n->b,expected);
    else
        stored=expected ? expected : unknown(s->c);
    actual=n->b && n->b->sem_type ? n->b->sem_type : stored;
    if (!expected) expected=stored;
    if (!n->a && n->b && fe_m7_is_null(n->b))
        err(s->c,n->loc,"null initializer requires an explicit optional type");
    if (expected && expected->kind==FE_TYPE_VOID)
        err(s->c,n->loc,"variable cannot have void type");
    if (n->b && !fe_type_equal(expected,stored) &&
        !m7_actual_compatible(expected,stored,n->b)) {
        /* Say which rule was hit. Weakening &mut to & is a distinct thing from
           two unrelated types not matching, and "type mismatch" told the reader
           nothing about why the exclusive borrow could not be shared. */
        if (expected && stored && expected->kind==FE_TYPE_REF &&
            stored->kind==FE_TYPE_REF && !expected->ref_mut && stored->ref_mut)
            err(s->c,n->loc,
                "cannot rebind a mut borrow as a shared reference");
        else if (expected && stored && expected->kind==FE_TYPE_SLICE &&
                 stored->kind==FE_TYPE_SLICE && !expected->ref_mut &&
                 stored->ref_mut)
            err(s->c,n->loc,
                "cannot rebind a mut slice as a shared slice");
        else
            err(s->c,n->loc,"initializer type mismatch");
    }
    /* Rules the M6 declaration case carried that this one has to repeat now
       that it is the only declaration case. */
    if (n->b && stored && stored->kind==FE_TYPE_VOID)
        err(s->c,n->loc,"void expression cannot initialize a variable");
    if (!mutable && expected && expected->kind==FE_TYPE_SLICE &&
        expected->ref_mut)
        err(s->c,n->loc,"let cannot bind a mutable slice");
    if (!n->b && !n->a)
        err(s->c,n->loc,"uninitialized var requires an explicit type");
    if (n->b) mark_moved(s,n->b,actual);
    initialized=n->b!=0;
    sym=add_symbol(s,s->scope,n->text,expected,0,mutable,initialized,
                   local_cname(s->c,n->text ? n->text : "local"),n);
    if (sym && n->b && n->b->kind==FE_N_UNARY && n->b->text &&
        (strcmp(n->b->text,"&")==0 || strcmp(n->b->text,"&mut")==0)) {
        sym->borrow_root=own_root_symbol(s,n->b->a);
        sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
        sym->borrow_defer=s->defer_depth!=0 ||
            own_defer_uses(s->fn_node ? s->fn_node->c : 0,n->text);
    }
    own_bind_derived_call(s,sym,n->b);
}

static void check_stmt(FeCheckerState *s, FeNode *n)
{
    FeScope *old;
    FeNode *x;
    FeType *expected;
    FeType *actual;
    FeType *stored;
    FeSym *sym;
    if (!n) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        old=s->scope;
        s->scope=scope_new(s,old);
        for (x=n->children;x;x=x->next) {
            check_stmt(s,x);
            own_release_after_stmt(s,s->scope,x,0);
        }
        own_release_after_stmt(s,s->scope,n,1);
        s->scope=old;
        break;
    case FE_N_LET:
    case FE_N_CONST:
        m7_check_decl_stmt(s,n,0);
        break;
    case FE_N_VAR:
        if (!n->a && !n->b)
            err(s->c,n->loc,"uninitialized var requires an explicit type");
        m7_check_decl_stmt(s,n,1);
        break;
    case FE_N_ASSIGN:
        expected=check_lvalue(s,n->a,compound_operator(n->text));
        stored=m7_check_expected(s,n->b,expected);
        actual=n->b && n->b->sem_type ? n->b->sem_type : stored;
        if (!fe_type_equal(expected,stored) &&
            !m7_actual_compatible(expected,stored,n->b))
            err(s->c,n->loc,"assignment type mismatch");
        mark_moved(s,n->b,actual);
        sym=n->a && n->a->kind==FE_N_IDENT ?
            find_symbol(s->scope,n->a->text) : 0;
        if (sym && sym->mutable) {
            sym->initialized=1;
            fe_own_access(s->c->diags,&sym->own,FE_OWN_WRITE,n->a->loc);
            sym->moved=sym->own.move;
            /* Rebinding a reference, from the M6 assignment case: the new
               source has to live at least as long as the reference does, and
               the previous borrow has to be released. */
            if (n->b && n->b->kind==FE_N_UNARY && n->b->text &&
                (strcmp(n->b->text,"&")==0 || strcmp(n->b->text,"&mut")==0) &&
                fe_own_is_reference_like(sym->type)) {
                FeSym *root=own_root_symbol(s,n->b->a);
                if (root && root->owner!=sym->owner)
                    err(s->c,n->b->loc,"reference would outlive its source scope");
                else if (root) {
                    if (sym->borrow_root) {
                        if (sym->borrow_mut)
                            fe_own_release_exclusive(&sym->borrow_root->own);
                        else fe_own_release_shared(&sym->borrow_root->own);
                    }
                    sym->borrow_root=root;
                    sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
                }
            }
        }
        break;
    case FE_N_EXPR_STMT:
        check_expr(s,n->a);
        break;
    case FE_N_DEFER:
        ++s->defer_depth;
        check_stmt(s,n->a);
        --s->defer_depth;
        break;
    case FE_N_IF:
        if (n->text && strcmp(n->text,"comptime if")==0) {
            int taken=0;
            if (!comptime_condition(s,n->a,&taken)) {
                err(s->c,n->a?n->a->loc:n->loc,
                    "comptime condition must be decidable at compile time");
                break;
            }
            /* SPEC 9: the branch that is not taken is parsed and nothing more. */
            if (taken) check_stmt(s,n->b);
            else if (n->c) check_stmt(s,n->c);
            break;
        }
        if (n->text && strcmp(n->text,"if let")==0)
            m7_check_if_let(s,n);
        else {
            FeType *cond;
            FeFlowSlot base[FE_M7_FLOW_CAP];
            FeFlowSlot left[FE_M7_FLOW_CAP];
            FeFlowSlot right[FE_M7_FLOW_CAP];
            FeOwnState *own_base;
            FeOwnState *own_left;
            FeOwnState *own_right;
            FeFlowBorrow *borrow_base;
            FeFlowBorrow *borrow_left;
            FeFlowBorrow *borrow_right;
            unsigned count;
            unsigned i;
            cond=check_expr(s,n->a);
            if (known(cond) && cond->kind!=FE_TYPE_BOOL)
                err(s->c,n->loc,"if condition must be bool");
            /* Once a function is on the M7 checker path, branch bodies must
               remain on that path as well.  In particular, return T inside
               E!T relies on contextual success construction even when the
               branch itself contains no surface M7 syntax. */
            m7_capture_flow(s,base,&own_base,&borrow_base,&count);
            check_stmt(s,n->b);
            flow_capture(s->scope,left,count);
            own_left=flow_own_new(s,count);
            borrow_left=flow_borrow_new(s,count);
            flow_own_capture(left,own_left,count);
            flow_borrow_capture(left,borrow_left,count);
            /* A branch that always leaves contributes nothing to what follows.
               Merging its state would make a value it consumed look consumed
               afterwards, on a path that never ran it. */
            if (m7_stmt_definitely_exits(n->b)) {
                for (i=0;i<count;++i) left[i]=base[i];
                if (own_left && own_base)
                    for (i=0;i<count;++i) own_left[i]=own_base[i];
                if (borrow_left && borrow_base)
                    for (i=0;i<count;++i) borrow_left[i]=borrow_base[i];
            }
            m7_restore_flow(base,own_base,borrow_base,count);
            if (n->c) check_stmt(s,n->c);
            if (n->c) {
                flow_capture(s->scope,right,count);
                own_right=flow_own_new(s,count);
                borrow_right=flow_borrow_new(s,count);
                flow_own_capture(right,own_right,count);
                flow_borrow_capture(right,borrow_right,count);
                if (m7_stmt_definitely_exits(n->c)) {
                    for (i=0;i<count;++i) right[i]=base[i];
                    if (own_right && own_base)
                        for (i=0;i<count;++i) own_right[i]=own_base[i];
                    if (borrow_right && borrow_base)
                        for (i=0;i<count;++i) borrow_right[i]=borrow_base[i];
                }
            } else {
                own_right=flow_own_new(s,count);
                borrow_right=flow_borrow_new(s,count);
                for (i=0;i<count;++i) right[i]=base[i];
                if (own_right && own_base)
                    for (i=0;i<count;++i) own_right[i]=own_base[i];
                if (borrow_right && borrow_base)
                    for (i=0;i<count;++i) borrow_right[i]=borrow_base[i];
            }
            flow_merge(base,left,right,count);
            flow_own_merge(base,own_left,own_right,count);
            flow_borrow_merge(base,borrow_left,borrow_right,count);
        }
        break;
    case FE_N_MATCH:
        m7_check_match_stmt(s,n);
        break;
    case FE_N_RETURN:
        expected=s->ret;
        if (n->a)
            stored=m7_check_expected(s,n->a,expected);
        else
            stored=fe_type_intern(&s->c->types,"void");
        actual=n->a && n->a->sem_type ? n->a->sem_type : stored;
        /* R8, from the M6 return case: a returned reference has to come from a
           parameter or a static, never from a local. */
        if (expected && fe_own_is_reference_like(expected) &&
            !own_return_from_allowed_root(s,n->a))
            err(s->c,n->loc,
                "reference return must be derived from a parameter or static");
        if (n->a && stored && stored->kind==FE_TYPE_VOID &&
            expected && expected->kind!=FE_TYPE_VOID)
            err(s->c,n->loc,"void expression returned from value function");
        if (expected && expected->kind==FE_TYPE_ERROR_UNION && n->a &&
            actual && actual->kind==FE_TYPE_ERROR_UNION &&
            !fe_type_equal(expected,actual))
            err(s->c,n->loc,"error result type mismatch");
        /* A bare `return` in a function returning `!void` is the success case:
           there is no value to give, and no error either. */
        else if (!n->a && expected && expected->kind==FE_TYPE_ERROR_UNION &&
                 expected->error_value &&
                 expected->error_value->kind==FE_TYPE_VOID) { }
        else if (!fe_type_equal(expected,stored) &&
                 !m7_actual_compatible(expected,stored,n->a))
            err(s->c,n->loc,"return type mismatch");
        if (n->a) mark_moved(s,n->a,actual);
        break;
    case FE_N_WHILE:
    case FE_N_FOR:
        /* The core loop case carries the flow capture and merge that detects a
           value moved on every iteration, and it already recurses into the body
           through this function, so there is nothing to special-case here. The
           M7 half used to skip all of it. */
        check_stmt_core(s,n);
        break;
    case FE_N_BREAK:
    case FE_N_CONTINUE:
        if (!s->loop_depth)
            err(s->c,n->loc,"break or continue outside loop");
        break;
    case FE_N_UNSAFE:
        check_stmt(s,n->a);
        break;
    default:
        check_stmt_core(s,n);
        break;
    }
}

static int m7_ast_reference_storage(FeNode *type)
{
    if (!type || !type->text) return 0;
    if (strcmp(type->text,"&")==0 || strcmp(type->text,"&mut")==0 ||
        (strcmp(type->text,"[")==0 && !type->a) ||
        strcmp(type->text,"str")==0)
        return 1;
    if (strcmp(type->text,"?")==0)
        return m7_ast_reference_storage(type->a);
    if (strcmp(type->text,"^")==0) return 0;
    return 0;
}

static void m7_check_storage(FeCheck *c, FeNode *decl)
{
    FeNode *m;
    if (!decl) return;
    if (decl->kind==FE_N_STRUCT || decl->kind==FE_N_ENUM) {
        /* This pass exists for the shapes the other one cannot see, such as a
           reference behind an optional. A plain `&T` field is seen by both, so
           leave that one to check_reference_storage below. */
        for (m=decl->children;m;m=m->next)
            if (m->kind==FE_N_FIELD && m7_ast_reference_storage(m->a) &&
                !own_ast_reference_type(m->a) &&
                !own_ast_pointer_to_reference(m->a))
                err(c,m->loc,"reference type is not allowed in aggregate storage");
    }
    check_reference_storage(c,decl);
}

static void m7_validate_error_decl(FeCheck *c, FeNode *decl)
{
    FeNode *a;
    FeNode *b;
    unsigned long code;
    unsigned long other;
    if (!decl || decl->kind!=FE_N_ERROR_DECL) return;
    for (a=decl->children;a;a=a->next) {
        if (!a->a || a->a->kind!=FE_N_LITERAL || !a->a->text) continue;
        code=strtoul(a->a->text,0,0);
        if (code==0UL)
            err(c,a->loc,"error code 0 is reserved for success");
        for (b=decl->children;b && b!=a;b=b->next) {
            if (a->text && b->text && strcmp(a->text,b->text)==0) {
                err(c,a->loc,"duplicate error member name");
                break;
            }
            if (b->a && b->a->kind==FE_N_LITERAL && b->a->text) {
                other=strtoul(b->a->text,0,0);
                if (other==code) {
                    err(c,a->loc,"duplicate error numeric code");
                    break;
                }
            }
        }
    }
}

/* Everything a unit declares, before any body anywhere is looked at. */
static void declare_unit(FeCheck *c)
{
    FeNode *n;
    /* A generic declaration is not a type; only its instances are. */
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_STRUCT && !decl_is_generic(n))
            fe_type_declare_struct(&c->types,n,(n->flags & FE_NODE_PACKED)!=0);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next) {
        FeNode *m;
        m7_check_storage(c,n);
        if (n->kind==FE_N_ERROR_DECL) m7_validate_error_decl(c,n);
        check_generic_params(c,n);
        for (m=n->kind==FE_N_STRUCT ? n->children : 0;m;m=m->next)
            if (m->kind==FE_N_FN) check_generic_params(c,m);
    }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_ENUM && !decl_is_generic(n))
            fe_type_declare_enum(&c->types,n);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_ERROR_DECL) fe_type_declare_error(&c->types,n);
    check_type_cycles(c);
}

/* The unit's top-level names, in a scope of their own so that another unit
   can look into it later without inheriting anything else. */
static FeScope *declare_unit_scope(FeCheck *c, FeCheckerState *s)
{
    FeNode *n;
    FeNode *m;
    FeType *t;
    FeScope *globals;
    char method_name[128];
    globals=scope_new(s,0);
    s->scope=globals;
    s->globals=globals;
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next) {
        if (n->kind==FE_N_STRUCT) {
            for (m=n->children;m;m=m->next) if (m->kind==FE_N_FN) {
                sprintf(method_name,"%s_%s",n->text ? n->text : "Type",
                        m->text ? m->text : "method");
                m->cname=unit_cname(c,method_name);
            }
        }
        if (n->kind==FE_N_GLOBAL || n->kind==FE_N_CONST) {
            t=n->a ? node_type(c,n->a) : unknown(c);
            add_symbol(s,globals,n->text,t,0,n->kind==FE_N_GLOBAL,
                       n->b!=0,unit_cname(c,n->text ? n->text : "global"),n);
        }
    }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_FN) {
            t=fe_type_intern(&c->types,"<fn>");
            /* `extern "c"` means the linker already knows this name, so it is
               not decorated with the unit it was declared in. */
            add_symbol(s,globals,n->text,t,n,0,1,
                       (n->flags & FE_NODE_EXTERN) && n->text ? n->text :
                       unit_cname(c,n->text ? n->text : "fn"),n);
        }
    return globals;
}

static void check_unit_bodies(FeCheck *c, FeCheckerState *s)
{
    FeNode *n;
    FeNode *m;
    FeSym *sym;
    FeType *t;
    FeType *iv;
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_GLOBAL || n->kind==FE_N_CONST) {
            sym=find_current(s->globals,n->text ? n->text : "");
            if (n->kind==FE_N_CONST && const_names_type(s,n)) continue;
            if (n->b) {
                iv=m7_check_expected(s,n->b,sym ? sym->type : 0);
                if (sym && sym->type->kind==FE_TYPE_UNKNOWN) {
                    sym->type=iv;
                    n->sem_type=iv;
                } else if (sym && !fe_type_equal(sym->type,iv) &&
                           !m7_actual_compatible(sym->type,iv,n->b))
                    err(c,n->loc,"global initializer type mismatch");
            }
        }
    /* A generic body means nothing until its parameters are bound, so it is
       checked once per instance and not here. */
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_FN && !decl_is_generic(n)) check_fn(c,n,s->globals);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_STRUCT && !decl_is_generic(n)) {
            t=fe_type_intern(&c->types,n->text);
            for (m=n->children;m;m=m->next)
                if (m->kind==FE_N_FN) check_method(c,m,s->globals,t);
        }
}

int fe_check_program(FeCheck *c)
{
    FeCheckerState s;
    unsigned u;
    s.c=c;
    s.scope=0;
    s.globals=0;
    s.ret=fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    s.fn_node=0;
    fe_own_liveness_init(&s.liveness,&c->arena);
    for (u=0;u<c->build->count;++u) { enter_unit(c,u); declare_unit(c); }
    fe_type_layout_all(&c->types);
    for (u=0;u<c->build->count;++u) {
        enter_unit(c,u);
        c->unit_scope[u]=declare_unit_scope(c,&s);
    }
    for (u=0;u<c->build->count;++u) {
        enter_unit(c,u);
        s.scope=c->unit_scope[u];
        s.globals=c->unit_scope[u];
        check_unit_bodies(c,&s);
    }
    fe_type_layout_all(&c->types);
    return c->diags->errors==0;
}

