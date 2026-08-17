#include "checkpri.h"

int m7_ast_reference_storage(FeNode *type)
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

void m7_check_storage(FeCheck *c, FeNode *decl)
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

void m7_validate_error_decl(FeCheck *c, FeNode *decl)
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
void declare_unit(FeCheck *c)
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
}

/* The unit's top-level names, in a scope of their own so that another unit
   can look into it later without inheriting anything else. */
FeScope *declare_unit_scope(FeCheck *c, FeCheckerState *s)
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

void check_unit_bodies(FeCheck *c, FeCheckerState *s)
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
                if (!const_foldable(s,n->b))
                    err(c,n->b->loc,
                        "a global initializer must be known at compile time");
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
    s.unsafe_depth=0;
    s.fn_node=0;
    fe_own_liveness_init(&s.liveness,&c->arena);
    for (u=0;u<c->build->count;++u) { enter_unit(c,u); declare_unit(c); }
    /* Only now: a field may name a type in a unit that had not declared it
       yet, and resolving it early would freeze the wrong answer in place. */
    for (u=0;u<c->build->count;++u) { enter_unit(c,u); check_type_cycles(c); }
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
