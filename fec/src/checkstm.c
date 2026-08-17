#include "checkpri.h"

void check_match(FeCheckerState *s, FeNode *n)
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

void check_for(FeCheckerState *s, FeNode *n)
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
        /* Walking a container borrows it for the length of the walk: the
           item is a reference into it, so writing the container underneath
           would move what that reference points at (SPEC 5 R6). */
        if (iter_sym)
            fe_own_access(s->c->diags,&iter_sym->own,
                          iter_mut ? FE_OWN_BORROW_MUT : FE_OWN_BORROW_SHARED,
                          n->loc);
        check_stmt(s,n->b);
        if (iter_sym) {
            if (iter_mut) fe_own_release_exclusive(&iter_sym->own);
            else fe_own_release_shared(&iter_sym->own);
        }
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

void check_type_cycle(FeCheck *c, FeType *t)
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
        int back=enter_declaring_unit(c,t->unit);
        for (i=0;i<t->field_count;i++)
            if (!t->fields[i].type && t->fields[i].ast_node)
                t->fields[i].type=fe_type_from_ast(&c->types,t->fields[i].ast_node->a);
        if (back>=0) enter_unit(c,(unsigned)back);
        for (i=0;i<t->field_count;i++) check_type_cycle(c,t->fields[i].type);
    } else if (t->kind == FE_TYPE_ENUM) {
        int back=enter_declaring_unit(c,t->unit);
        for (i=0;i<t->variant_count;i++) {
            unsigned j;
            for (j=0;j<t->variants[i].field_count;j++)
                if (!t->variants[i].fields[j].type && t->variants[i].fields[j].ast_node)
                    t->variants[i].fields[j].type=fe_type_from_ast(&c->types,
                        t->variants[i].fields[j].ast_node->a);
        }
        if (back>=0) enter_unit(c,(unsigned)back);
        for (i=0;i<t->variant_count;i++) {
            unsigned j;
            for (j=0;j<t->variants[i].field_count;j++) {
                next=t->variants[i].fields[j].type;
                check_type_cycle(c,next);
            }
        }
    }
    t->cycle_state=2;
}

void check_type_cycles(FeCheck *c)
{
    FeType *t;
    for (t=c->types.types;t;t=t->next) t->cycle_state=0;
    for (t=c->types.types;t;t=t->next) check_type_cycle(c,t);
}

int own_ast_reference_type(FeNode *type)
{
    if (!type || !type->text) return 0;
    return strcmp(type->text,"&")==0 || strcmp(type->text,"&mut")==0 ||
        (strcmp(type->text,"[")==0 && !type->a) || strcmp(type->text,"str")==0;
}

int own_ast_pointer_to_reference(FeNode *type)
{
    return type && type->text && strcmp(type->text,"*")==0 &&
        own_ast_reference_type(type->a);
}

void check_reference_storage(FeCheck *c, FeNode *decl)
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

int own_return_from_allowed_root(FeCheckerState *s, FeNode *expr)
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

void check_stmt_core(FeCheckerState *s, FeNode *n)
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
            sym->borrow_field=own_projected_field(n->b->a,0);
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
            sym->borrow_field=own_projected_field(n->b->a,0);
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
        /* A deferred block runs during scope cleanup, on the way out of a
           function that has already decided what it returns. There is nothing
           for a `return` in there to mean. */
        if (s->defer_depth != 0)
            err(c, n->loc, "cannot return from inside defer");
        b = n->a ? check_expr(s, n->a) : fe_type_intern(&c->types, "void");
        if (s->ret && fe_own_is_reference_like(s->ret) &&
            !own_return_from_allowed_root(s,n->a))
            err(c,n->loc,"reference return must be derived from a parameter or static");
        mark_moved(s,n->a,b);
        if (known(b) && b->kind == FE_TYPE_VOID && s->ret->kind != FE_TYPE_VOID)
            err(c, n->loc, "void expression returned from value function");
        else if (return_weakens(s->ret,b)) { }
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

void check_fn(FeCheck *c, FeNode *fn, FeScope *globals)
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

void check_method(FeCheck *c, FeNode *fn, FeScope *globals,
                         FeType *owner)
{
    FeCheckerState s;
    FeNode *x;
    FeType *t;
    FeBindSave self_save;
    /* `Self` names the type a method belongs to, wherever it appears -- in a
       signature, and in `Self{ .. }`. Binding it as a type makes both work the
       same way, and the same way a generic instance already worked. */
    self_save.count=c->types.param_count;
    {
        unsigned i;
        for(i=0;i<FE_TYPE_PARAM_MAX;++i) self_save.params[i]=c->types.params[i];
    }
    bind_self(c,owner);
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
    pop_bindings(c,&self_save);
}

int m7_actual_compatible(FeType *want, FeType *got, FeNode *value)
{
    if (fe_type_equal(want,got)) return 1;
    return compatible(want,got,value);
}

/* The magnitude an integer literal spells, ignoring any sign. The same shape
   lowering uses on the same text, so the two cannot disagree about what was
   written. */
static unsigned long literal_magnitude(const char *s)
{
    unsigned long v = 0;
    if (!s) return 0;
    if (s[0]=='0' && (s[1]=='x' || s[1]=='X')) {
        for (s += 2; *s; ++s) {
            int d = *s>='0'&&*s<='9' ? *s-'0' :
                    *s>='a'&&*s<='f' ? *s-'a'+10 :
                    *s>='A'&&*s<='F' ? *s-'A'+10 : -1;
            if (d < 0) { if (*s=='_') continue; break; }
            v = v*16UL + (unsigned long)d;
        }
        return v;
    }
    for (; *s; ++s) {
        if (*s=='_') continue;
        if (*s<'0' || *s>'9') break;
        v = v*10UL + (unsigned long)(*s-'0');
    }
    return v;
}

/* SPEC 4.1: an integer literal takes the type its context asks for, and a
   value that does not fit that type is a mistake where it is written rather
   than a truncation nobody sees. */
static int literal_fits(const FeType *want, const char *text, int negative)
{
    unsigned long v;
    unsigned long limit;
    unsigned bits;
    if (!want || want->kind != FE_TYPE_INT || !text) return 1;
    bits = want->bits ? want->bits : 32U;
    if (bits > 32U) bits = 32U;
    v = literal_magnitude(text);
    if (want->is_unsigned) {
        if (negative) return v == 0UL;
        if (bits >= 32U) return 1;
        return v <= (1UL << bits) - 1UL;
    }
    limit = bits >= 32U ? 2147483647UL : (1UL << (bits - 1U)) - 1UL;
    return v <= (negative ? limit + 1UL : limit);
}

/* Is this node a plain integer literal, rather than a character, a string, or
   one of the word-shaped literals? */
static int plain_int_literal(const FeNode *n)
{
    return n && n->kind==FE_N_LITERAL && n->text &&
           n->text[0]!='\'' && n->text[0]!='"' &&
           strcmp(n->text,"true") && strcmp(n->text,"false") &&
           strcmp(n->text,"null") && strcmp(n->text,"undefined");
}

FeType *m7_check_expected(FeCheckerState *s, FeNode *value,
                                 FeType *expected)
{
    FeType *actual;
    FeM7ContextKind context;
    if (!value) return unknown(s->c);
    /* `undefined` is not a value, it is the absence of one: it takes whatever
       type was asked for, and says the storage starts out unset. Without this
       there is no way to declare a buffer larger than you care to type out. */
    if (value->kind==FE_N_LITERAL && value->text &&
        !strcmp(value->text,"undefined") && expected) {
        value->sem_type=expected;
        return expected;
    }
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
    /* An integer literal is `i32` on its own; where an integer type is asked
       for it is that type instead, and it has to fit in it. */
    if (expected && expected->kind==FE_TYPE_INT) {
        FeNode *lit = plain_int_literal(value) ? value :
            (value->kind==FE_N_UNARY && value->text &&
             !strcmp(value->text,"-") && plain_int_literal(value->a)
             ? value->a : 0);
        if (lit) {
            if (!literal_fits(expected, lit->text, lit!=value))
                err(s->c,value->loc,"integer literal out of range for its type");
            lit->sem_type=expected;
            value->sem_type=expected;
            return expected;
        }
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

FeType *m7_member_field(FeCheckerState *s, FeNode *n, FeType *base)
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

int m7_place_is_projection(FeNode *n)
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

