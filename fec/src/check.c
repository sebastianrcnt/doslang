#include "checkpri.h"

FeType *unknown(FeCheck *c)
{
    return fe_type_intern(&c->types, "<unknown>");
}

void err(FeCheck *c, FeLoc loc, const char *msg)
{
    fe_diag_error(c->diags, loc, msg);
}

/* Only numbers and characters have an order (SPEC 6.2). */
int ordered_type(const FeType *t)
{
    return t && (t->kind==FE_TYPE_INT || t->kind==FE_TYPE_CHAR);
}

int known(FeType *t)
{
    return t && t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_ERROR;
}

/* Is this a projection of `self` inside that type's own `drop`? */
int in_own_drop(FeCheckerState *s, FeNode *n)
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

void mark_moved(FeCheckerState *s, FeNode *n, FeType *t)
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

int compatible(FeType *want, FeType *got, FeNode *value)
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
int call_reborrows(const FeType *param, const FeType *arg)
{
    if (!param || !arg) return 0;
    if (param->kind==FE_TYPE_REF && arg->kind==FE_TYPE_REF &&
        param->ref_mut && arg->ref_mut) return 1;
    if (param->kind==FE_TYPE_SLICE && arg->kind==FE_TYPE_SLICE &&
        param->ref_mut && arg->ref_mut) return 1;
    return 0;
}

int explicit_castable(FeType *a, FeType *b)
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

FeType *node_type(FeCheck *c, FeNode *n)
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
char *unit_cname(FeCheck *c, const char *name)
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

char *local_cname(FeCheck *c, const char *name)
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

FeScope *scope_new(FeCheckerState *s, FeScope *parent)
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

FeSym *find_current(FeScope *scope, const char *name)
{
    unsigned i;
    if (!scope) return 0;
    for (i = scope->count; i > 0; --i)
        if (strcmp(scope->items[i - 1].name, name) == 0)
            return &scope->items[i - 1];
    return 0;
}

FeSym *find_symbol(FeScope *scope, const char *name)
{
    FeSym *sym;
    while (scope) {
        sym = find_current(scope, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return 0;
}

FeSym *add_symbol(FeCheckerState *s, FeScope *scope,
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
void enter_unit(FeCheck *c, unsigned index)
{
    FeUnit *u = &c->build->units[index];
    c->unit = u;
    c->ast = &u->ast;
    c->types.unit_name = u->name[0] ? u->name : "unit";
    fe_diags_source(c->diags, u->source, u->size);
}



static int enter_decl_hook(void *owner, const char *unit);
static void leave_decl_hook(void *owner, int back);

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
    c->types.enter_decl = enter_decl_hook;
    c->types.leave_decl = leave_decl_hook;
    c->instances = (FeInstance *)fe_arena_alloc(&c->arena,
        (unsigned long)FE_GENERIC_INSTANCE_MAX * sizeof(FeInstance));
    c->instance_count = 0;
    c->instance_depth = 0;
}

void fe_check_destroy(FeCheck *c)
{
    fe_arena_destroy(&c->arena);
}

unsigned unit_index(FeCheck *c, const FeUnit *u)
{
    return (unsigned)(u - c->build->units);
}

/* An import introduces a local binding, so `binding.name` reaches into the
   unit it names. A local of the same spelling wins -- shadowing a binding is
   legal and means the local -- so this only answers when the base name is not
   otherwise in scope. */
FeUnit *binding_unit(FeCheckerState *s, FeNode *base)
{
    if (!base || base->kind!=FE_N_IDENT || !base->text) return 0;
    if (!s->c->build || !s->c->unit) return 0;
    if (find_symbol(s->scope,base->text)) return 0;
    return fe_build_binding(s->c->build,s->c->unit,base->text);
}

/* SPEC 8.2: a declaration is visible outside its unit only with `pub`. */
int decl_is_public(const FeNode *decl)
{
    return decl && (decl->flags & FE_NODE_PUB)!=0;
}

FeSym *unit_member(FeCheck *c, FeUnit *u, const char *name)
{
    if (!u || !name) return 0;
    return find_current(c->unit_scope[unit_index(c,u)],name);
}

/* A type another unit declares, or null if it declares no such type. Interning
   is keyed on the declaring unit, so this cannot collide with a same-named
   type here. */
FeType *unit_type(FeCheck *c, FeUnit *u, const char *name)
{
    FeType *t;
    if (!u || !name) return 0;
    for (t=c->types.types;t;t=t->next)
        if (t->unit && strcmp(t->name,name)==0 &&
            strcmp(t->unit,u->name)==0 && t->kind!=FE_TYPE_UNKNOWN) return t;
    return 0;
}

/* A field type is written in the unit that declared the type, so it has to be
   resolved with that unit's imports in scope -- not with whichever unit
   happens to be current when the walk reaches it. Returns the index to go back
   to, or -1 when there is nowhere to go. */
int enter_declaring_unit(FeCheck *c, const char *unit_name)
{
    unsigned i;
    unsigned here;
    if (!unit_name || !c->build || !c->unit) return -1;
    here = unit_index(c,c->unit);
    for (i=0;i<c->build->count;++i)
        if (strcmp(c->build->units[i].name,unit_name)==0) {
            if (i==here) return -1;
            enter_unit(c,i);
            return (int)here;
        }
    return -1;
}

/* The type layer calls these; it knows nothing about units beyond a name. */
static int enter_decl_hook(void *owner, const char *unit)
{
    return enter_declaring_unit((FeCheck *)owner, unit);
}

static void leave_decl_hook(void *owner, int back)
{
    enter_unit((FeCheck *)owner, (unsigned)back);
}

/* The AST declaration of a type another unit declares, for its visibility and
   for its methods. */
FeNode *unit_type_decl(FeCheck *c, FeUnit *u, const char *name)
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
FeType *node_type_in(FeCheck *c, const char *unit, FeNode *node)
{
    const char *save=c->types.unit_name;
    FeType *t;
    if (unit) c->types.unit_name=unit;
    t=node_type(c,node);
    c->types.unit_name=save;
    return t;
}


FeNode *find_method(FeCheck *c, FeType *owner, const char *name)
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

FeType *method_type(FeCheck *c, FeNode *node, FeType *owner)
{
    if(node && node->kind==FE_N_TYPE && node->text &&
       strcmp(node->text,"Self")==0) return owner;
    if(node && node->kind==FE_N_TYPE && node->text &&
       (strcmp(node->text,"&")==0 || strcmp(node->text,"&mut")==0) &&
       node->a && node->a->text && strcmp(node->a->text,"Self")==0)
        return fe_type_ref(&c->types,owner,strcmp(node->text,"&mut")==0);
    return node_type(c,node);
}


unsigned flow_capture(FeScope *scope, FeFlowSlot *slots, unsigned cap)
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

void flow_restore(FeFlowSlot *slots, unsigned count)
{
    unsigned i;
    for (i=0; i<count; ++i) {
        slots[i].sym->moved=slots[i].moved;
        slots[i].sym->initialized=slots[i].initialized;
        slots[i].sym->own.move=slots[i].own_move;
        slots[i].sym->own.initialized=slots[i].own_initialized;
    }
}

void flow_merge(FeFlowSlot *base, FeFlowSlot *left, FeFlowSlot *right,
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

FeSym *own_root_symbol(FeCheckerState *s, FeNode *expr)
{
    FeOwnPlace place;
    if (!fe_own_place_from_expr(expr,&place)) return 0;
    return find_symbol(s->scope,place.root->text ? place.root->text : "");
}

int own_is_global(FeCheckerState *s, FeSym *sym)
{
    FeScope *p;
    if (!s || !sym) return 0;
    for (p=s->globals; p; p=p->parent) {
        unsigned i;
        for (i=0;i<p->count;++i) if (&p->items[i]==sym) return 1;
    }
    return 0;
}

void own_borrow_expr(FeCheckerState *s, FeNode *expr, int mutable)
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

void own_release_temporary_borrow(FeCheckerState *s, FeNode *expr)
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
FeSym *own_derived_call_root(FeCheckerState *s, FeNode *call)
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

void own_bind_derived_call(FeCheckerState *s, FeSym *binding,
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

int own_stmt_uses(FeNode *node, const char *name)
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

int own_defer_uses(FeNode *node, const char *name)
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

int own_contains_node(FeNode *node, FeNode *needle)
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

void own_release_after_stmt(FeCheckerState *s, FeScope *scope,
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
FeOwnState *flow_own_new(FeCheckerState *s, unsigned count)
{
    if (!s || !count) return 0;
    return (FeOwnState *)fe_arena_alloc(&s->c->arena,
                                        count*sizeof(FeOwnState));
}

void flow_own_capture(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) states[i]=slots[i].sym->own;
}

void flow_own_restore(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) slots[i].sym->own=states[i];
}

void flow_own_merge(FeFlowSlot *slots, FeOwnState *left,
                           FeOwnState *right, unsigned count)
{
    unsigned i;
    if (!left || !right) return;
    for (i=0;i<count;++i)
        slots[i].sym->own=fe_own_merge_state(left[i],right[i]);
}


FeFlowBorrow *flow_borrow_new(FeCheckerState *s, unsigned count)
{
    if (!s || !count) return 0;
    return (FeFlowBorrow *)fe_arena_alloc(&s->c->arena,
                                          count*sizeof(FeFlowBorrow));
}

void flow_borrow_capture(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) {
        states[i].root=slots[i].sym->borrow_root;
        states[i].mutable=slots[i].sym->borrow_mut;
    }
}

void flow_borrow_restore(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count)
{
    unsigned i;
    if (!states) return;
    for (i=0;i<count;++i) {
        slots[i].sym->borrow_root=states[i].root;
        slots[i].sym->borrow_mut=states[i].mutable;
    }
}

void flow_borrow_merge(FeFlowSlot *slots, FeFlowBorrow *left,
                              FeFlowBorrow *right, unsigned count)
{
    unsigned i;
    if (!left || !right) return;
    for (i=0;i<count;++i) {
        slots[i].sym->borrow_root=left[i].root ? left[i].root : right[i].root;
        slots[i].sym->borrow_mut=left[i].mutable || right[i].mutable;
    }
}
