#include "checkpri.h"

int is_error_set_member(FeCheckerState *s, FeNode *n)
{
    return n && n->kind==FE_N_MEMBER && n->a && n->a->kind==FE_N_IDENT &&
           n->a->text && strcmp(n->a->text,"error")==0 &&
           n->b && n->b->text && !find_symbol(s->scope,"error");
}

/* `binding.name` used as a value rather than called. */
FeType *cross_unit_value(FeCheckerState *s, FeNode *n, int *handled)
{
    FeUnit *home=binding_unit(s,n->a);
    FeSym *sym;
    *handled=0;
    if (!home) return 0;
    *handled=1;
    sym=unit_member(s->c,home,n->b && n->b->text ? n->b->text : "");
    if (!sym) {
        /* A name in another unit can be a type as well as a value --
           `binding.Enum.Variant` reaches one through the other. */
        FeType *there=unit_type(s->c,home,n->b && n->b->text ? n->b->text : "");
        FeNode *decl=unit_type_decl(s->c,home,
                                    n->b && n->b->text ? n->b->text : "");
        if (there && decl) {
            if (!decl_is_public(decl)) {
                err(s->c,n->loc,"type is private to its unit");
                return unknown(s->c);
            }
            n->sem_type=there;
            return there;
        }
        err(s->c,n->loc,"unknown name");
        return unknown(s->c);
    }
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
FeType *check_call_args(FeCheckerState *s, FeNode *n, FeSym *sym,
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

FeType *check_call(FeCheckerState *s, FeNode *n)
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
            own_release_temporary_borrow(s,arg);
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

void m7_capture_flow(FeCheckerState *s, FeFlowSlot *slots,
                            FeOwnState **own, FeFlowBorrow **borrow,
                            unsigned *count)
{
    *count=flow_capture(s->scope,slots,FE_M7_FLOW_CAP);
    *own=flow_own_new(s,*count);
    *borrow=flow_borrow_new(s,*count);
    flow_own_capture(slots,*own,*count);
    flow_borrow_capture(slots,*borrow,*count);
}

void m7_restore_flow(FeFlowSlot *slots, FeOwnState *own,
                            FeFlowBorrow *borrow, unsigned count)
{
    flow_restore(slots,count);
    flow_own_restore(slots,own,count);
    flow_borrow_restore(slots,borrow,count);
}

void m7_merge_rhs_flow(FeCheckerState *s, FeFlowSlot *base,
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

int m7_stmt_definitely_exits(FeNode *n)
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

FeType *m7_check_lazy(FeCheckerState *s, FeNode *n,
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

static FeType *check_expr_dispatch(FeCheckerState *s, FeNode *n);

/* Every expression goes through here, which is where a projection can tell
   the identifier underneath it which field is actually being reached. */
FeType *check_expr(FeCheckerState *s, FeNode *n)
{
    const char *save_field=s->proj_field;
    FeNode *save_base=s->proj_base;
    FeType *t;
    if (n && (n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX))
        s->proj_field=own_projected_field(n,&s->proj_base);
    t=check_expr_dispatch(s,n);
    s->proj_field=save_field;
    s->proj_base=save_base;
    return t;
}

static FeType *check_expr_dispatch(FeCheckerState *s, FeNode *n)
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
        /* A raw pointer plus a number is an address further along. Only raw
           pointers: an owner or a borrow has a place it belongs to, and
           walking away from it is what `*T` is for. */
        if (known(a) && a->kind==FE_TYPE_RAW && known(b) &&
            fe_type_is_integer(b) && op[0] && (op[0]=='+' || op[0]=='-')) {
            n->sem_type=a;
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

static FeType *check_lvalue_dispatch(FeCheckerState *s, FeNode *n, int read);

/* An assignment target is a projection too, so it leaves the same word for the
   identifier underneath: `self.room = x` reaches `room` and nothing else. */
FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read)
{
    const char *save_field=s->proj_field;
    FeNode *save_base=s->proj_base;
    FeType *t;
    if (n && (n->kind==FE_N_MEMBER || n->kind==FE_N_INDEX))
        s->proj_field=own_projected_field(n,&s->proj_base);
    t=check_lvalue_dispatch(s,n,read);
    s->proj_field=save_field;
    s->proj_base=save_base;
    return t;
}

static FeType *check_lvalue_dispatch(FeCheckerState *s, FeNode *n, int read)
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

FeType *m7_pattern_binding_type(FeCheckerState *s, FeType *payload,
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

void m7_check_if_let(FeCheckerState *s, FeNode *n)
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

void m7_check_optional_match(FeCheckerState *s, FeNode *n,
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

void m7_check_match_stmt(FeCheckerState *s, FeNode *n)
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

void m7_check_decl_stmt(FeCheckerState *s, FeNode *n, int mutable)
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
        sym->borrow_field=own_projected_field(n->b->a,0);
        sym->borrow_mut=strcmp(n->b->text,"&mut")==0;
        sym->borrow_defer=s->defer_depth!=0 ||
            own_defer_uses(s->fn_node ? s->fn_node->c : 0,n->text);
    }
    own_bind_derived_call(s,sym,n->b);
}

void check_stmt(FeCheckerState *s, FeNode *n)
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
        /* A deferred block runs during scope cleanup, on the way out of a
           function that has already decided what it returns. There is nothing
           for a `return` in there to mean. */
        if (s->defer_depth != 0)
            err(s->c, n->loc, "cannot return from inside defer");
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
                 !m7_actual_compatible(expected,stored,n->a) &&
                 !return_weakens(expected,stored))
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
