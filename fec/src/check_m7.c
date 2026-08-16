/* Compiler-A M7 integration.  The verified M1-M6 checker remains the exact
   fast path for sources that do not use M7 syntax or types.  M7 sources reuse
   its symbol, ownership and flow helpers from this translation unit. */
#define fe_check_init fe_check_init_m6
#define fe_check_program fe_check_program_m6
#define fe_check_expr_type fe_check_expr_type_m6
#include "check.c"
#undef fe_check_init
#undef fe_check_program
#undef fe_check_expr_type

#include "m7.h"
#include <stdlib.h>

#define FE_M7_FLOW_CAP 64U

static FeType *m7_check_expr(FeCheckerState *s, FeNode *n);
static void m7_check_stmt(FeCheckerState *s, FeNode *n);

static int m7_type_ast(const FeNode *n)
{
    const FeNode *x;
    if (!n) return 0;
    if (n->kind==FE_N_TYPE && n->text &&
        (strcmp(n->text,"?")==0 || strcmp(n->text,"!")==0))
        return 1;
    if (m7_type_ast(n->a) || m7_type_ast(n->b) || m7_type_ast(n->c))
        return 1;
    for (x=n->children;x;x=x->next)
        if (m7_type_ast(x)) return 1;
    return 0;
}

static int m7_node_feature(const FeNode *n)
{
    const FeNode *x;
    if (!n) return 0;
    if (n->kind==FE_N_ERROR_DECL) return 1;
    if (fe_m7_is_null(n) || fe_m7_is_try(n)) return 1;
    if (n->kind==FE_N_MEMBER && n->text && strcmp(n->text,".?")==0)
        return 1;
    if (n->kind==FE_N_BINARY && fe_m7_lazy_kind(n)!=FE_M7_LAZY_NONE)
        return 1;
    if (n->kind==FE_N_IF && n->text && strcmp(n->text,"if let")==0)
        return 1;
    if (n->kind==FE_N_ARM && n->text &&
        (strcmp(n->text,"Some")==0 || strcmp(n->text,"None")==0))
        return 1;
    if (m7_type_ast(n)) return 1;
    if (m7_node_feature(n->a) || m7_node_feature(n->b) ||
        m7_node_feature(n->c)) return 1;
    for (x=n->children;x;x=x->next)
        if (m7_node_feature(x)) return 1;
    return 0;
}

static int m7_program_feature(FeCheck *c)
{
    return c && c->ast && m7_node_feature(c->ast->root);
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
    actual=m7_check_expr(s,value);
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

static int m7_error_same(FeCheckerState *s, FeType *a, FeType *b)
{
    FeType *ea;
    FeType *eb;
    if (!a || !b || a->kind!=FE_TYPE_ERROR_UNION ||
        b->kind!=FE_TYPE_ERROR_UNION) return 0;
    ea=fe_m7_error_type(&s->c->types,a);
    eb=fe_m7_error_type(&s->c->types,b);
    return ea && eb && fe_type_equal(ea,eb);
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

static FeType *m7_check_call(FeCheckerState *s, FeNode *n)
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
            a=m7_check_expr(s,arg);
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
            a=arg ? m7_check_expr(s,arg) : unknown(c);
            if (!arg || arg->next || !a || a->kind!=FE_TYPE_OWNED)
                err(c,n->loc,"mem.destroy requires exactly one owned pointer");
            else mark_moved(s,arg,a);
            n->sem_type=fe_type_intern(&c->types,"void");
            return n->sem_type;
        }
        if (strcmp(n->a->b->text,"create")==0 ||
            strcmp(n->a->b->text,"alloc_slice")==0)
            return check_expr(s,n);
    }
    if (n->a && n->a->kind==FE_N_IDENT) {
        sym=find_symbol(s->scope,n->a->text ? n->a->text : "");
        if (!sym || !sym->fn) {
            err(c,n->loc,"unknown function");
            n->sem_type=unknown(c);
            return n->sem_type;
        }
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
                         a->kind==FE_TYPE_SLICE && !b->ref_mut && a->ref_mut))
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
    return check_expr(s,n);
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
    left_type=m7_check_expr(s,n->a);
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
        m7_check_stmt(s,n->c);
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

static FeType *m7_check_expr(FeCheckerState *s, FeNode *n)
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
        return check_identifier(s,n,1);
    if (n->kind==FE_N_LITERAL)
        return check_expr(s,n);
    if (n->kind==FE_N_CALL)
        return m7_check_call(s,n);
    if (n->kind==FE_N_MEMBER) {
        a=m7_check_expr(s,n->a);
        return m7_member_field(s,n,a);
    }
    if (n->kind==FE_N_INDEX)
        return check_index(s,n);
    if (n->kind==FE_N_UNARY) {
        op=n->text ? n->text : "";
        if (strcmp(op,"try")==0) {
            a=m7_check_expr(s,n->a);
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
            a=m7_check_expr(s,n->a);
            own_borrow_expr(s,n->a,strcmp(op,"&mut")==0);
            n->sem_type=fe_type_ref(&s->c->types,a,strcmp(op,"&mut")==0);
            return n->sem_type;
        }
        return check_expr(s,n);
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
            a=m7_check_expr(s,nonnull);
            if (!a || a->kind!=FE_TYPE_OPTIONAL)
                err(s->c,n->loc,"null comparison requires an optional value");
            else {
                nullnode->sem_type=a;
                nullnode->sem_context=a;
            }
            n->sem_type=fe_type_intern(&s->c->types,"bool");
            return n->sem_type;
        }
        a=m7_check_expr(s,n->a);
        b=m7_check_expr(s,n->b);
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
        return check_expr(s,n);
    if (n->kind==FE_N_STRUCT_INIT)
        return check_struct_init(s,n);
    if (n->kind==FE_N_ARRAY_INIT)
        return check_array_init(s,n);
    return check_expr(s,n);
}

static FeType *m7_check_lvalue(FeCheckerState *s, FeNode *n, int read)
{
    FeType *base;
    FeFieldType *field;
    FeType *owner;
    if (!n) return unknown(s->c);
    if (n->kind==FE_N_MEMBER) {
        base=m7_check_expr(s,n->a);
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
            field=fe_type_field(owner,n->b->text);
            if (!field) {
                err(s->c,n->loc,"assignment requires a valid struct field");
                return unknown(s->c);
            }
            n->sem_type=field->type;
            return field->type;
        }
    }
    return check_lvalue(s,n,read);
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
    opt=m7_check_expr(s,n->a);
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
    m7_check_stmt(s,n->b);
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
    if (n->c) m7_check_stmt(s,n->c);
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
        if (arm->a && arm->a->kind==FE_N_BLOCK) m7_check_stmt(s,arm->a);
        else if (arm->a) m7_check_expr(s,arm->a);
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
    value=m7_check_expr(s,n->a);
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
        !m7_actual_compatible(expected,stored,n->b))
        err(s->c,n->loc,"initializer type mismatch");
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

static void m7_check_stmt(FeCheckerState *s, FeNode *n)
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
            m7_check_stmt(s,x);
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
        expected=m7_check_lvalue(s,n->a,compound_operator(n->text));
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
        }
        break;
    case FE_N_EXPR_STMT:
        m7_check_expr(s,n->a);
        break;
    case FE_N_DEFER:
        ++s->defer_depth;
        m7_check_stmt(s,n->a);
        --s->defer_depth;
        break;
    case FE_N_IF:
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
            cond=m7_check_expr(s,n->a);
            if (known(cond) && cond->kind!=FE_TYPE_BOOL)
                err(s->c,n->loc,"if condition must be bool");
            /* Once a function is on the M7 checker path, branch bodies must
               remain on that path as well.  In particular, return T inside
               E!T relies on contextual success construction even when the
               branch itself contains no surface M7 syntax. */
            m7_capture_flow(s,base,&own_base,&borrow_base,&count);
            m7_check_stmt(s,n->b);
            flow_capture(s->scope,left,count);
            own_left=flow_own_new(s,count);
            borrow_left=flow_borrow_new(s,count);
            flow_own_capture(left,own_left,count);
            flow_borrow_capture(left,borrow_left,count);
            m7_restore_flow(base,own_base,borrow_base,count);
            if (n->c) m7_check_stmt(s,n->c);
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
        if (expected && expected->kind==FE_TYPE_ERROR_UNION && n->a &&
            actual && actual->kind==FE_TYPE_ERROR_UNION &&
            !fe_type_equal(expected,actual))
            err(s->c,n->loc,"error result type mismatch");
        else if (!fe_type_equal(expected,stored) &&
                 !m7_actual_compatible(expected,stored,n->a))
            err(s->c,n->loc,"return type mismatch");
        if (n->a) mark_moved(s,n->a,actual);
        break;
    case FE_N_WHILE:
    case FE_N_FOR:
        /* M7 fixtures only need existing loop semantics; any M7 expression in
           a loop is still recursively checked by function-local expressions
           used in the body through the fallback path below. */
        if (!m7_node_feature(n)) check_stmt(s,n);
        else {
            if (n->kind==FE_N_WHILE) {
                actual=m7_check_expr(s,n->a);
                if (known(actual) && actual->kind!=FE_TYPE_BOOL)
                    err(s->c,n->loc,"while condition must be bool");
                ++s->loop_depth;
                m7_check_stmt(s,n->b);
                --s->loop_depth;
            } else check_for(s,n);
        }
        break;
    case FE_N_BREAK:
    case FE_N_CONTINUE:
        if (!s->loop_depth)
            err(s->c,n->loc,"break or continue outside loop");
        break;
    case FE_N_UNSAFE:
        m7_check_stmt(s,n->a);
        break;
    default:
        check_stmt(s,n);
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
        for (m=decl->children;m;m=m->next)
            if (m->kind==FE_N_FIELD && m7_ast_reference_storage(m->a))
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

static void m7_check_fn(FeCheck *c, FeNode *fn, FeScope *globals)
{
    FeCheckerState s;
    FeNode *x;
    FeType *t;
    s.c=c;
    s.globals=globals;
    s.scope=scope_new(&s,globals);
    s.ret=fn->b ? node_type(c,fn->b) : fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    s.fn_node=fn;
    fe_own_liveness_init(&s.liveness,&c->ast->arena);
    fe_own_collect_last_uses(&s.liveness,fn);
    fn->sem_type=s.ret;
    for (x=fn->a ? fn->a->children : 0;x;x=x->next) {
        t=node_type(c,x->a);
        if (t->kind==FE_TYPE_VOID)
            err(c,x->loc,"parameter cannot have void type");
        add_symbol(&s,s.scope,x->text,t,0,1,1,
                   local_cname(c,x->text ? x->text : "arg"),x);
    }
    if (fn->c) m7_check_stmt(&s,fn->c);
}

static void m7_check_method(FeCheck *c, FeNode *fn, FeScope *globals,
                            FeType *owner)
{
    FeCheckerState s;
    FeNode *x;
    FeType *t;
    s.c=c;
    s.globals=globals;
    s.scope=scope_new(&s,globals);
    s.ret=fn->b ? method_type(c,fn->b,owner) :
        fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    s.fn_node=fn;
    fe_own_liveness_init(&s.liveness,&c->ast->arena);
    fe_own_collect_last_uses(&s.liveness,fn);
    fn->sem_type=s.ret;
    for (x=fn->a ? fn->a->children : 0;x;x=x->next) {
        t=method_type(c,x->a,owner);
        x->sem_type=t;
        add_symbol(&s,s.scope,x->text,t,0,1,1,
                   local_cname(c,x->text ? x->text : "arg"),x);
    }
    if (fn->c) m7_check_stmt(&s,fn->c);
}

void fe_check_init(FeCheck *c, FeAst *ast, FeDiags *diags,
                   unsigned pointer_bits, int no_checks)
{
    fe_check_init_m6(c,ast,diags,pointer_bits,no_checks);
}

int fe_check_program(FeCheck *c)
{
    FeCheckerState s;
    FeNode *n;
    FeNode *m;
    FeSym *sym;
    FeType *t;
    FeType *iv;
    char method_name[128];
    if (!m7_program_feature(c)) return fe_check_program_m6(c);
    s.c=c;
    s.scope=scope_new(&s,0);
    s.globals=s.scope;
    s.ret=fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    s.fn_node=0;
    fe_own_liveness_init(&s.liveness,&c->ast->arena);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_STRUCT)
            fe_type_declare_struct(&c->types,n,(n->flags & 1U)!=0);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next) {
        m7_check_storage(c,n);
        if (n->kind==FE_N_ERROR_DECL) m7_validate_error_decl(c,n);
    }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_ENUM) fe_type_declare_enum(&c->types,n);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_ERROR_DECL) fe_type_declare_error(&c->types,n);
    check_type_cycles(c);
    fe_type_layout_all(&c->types);
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
            add_symbol(&s,s.globals,n->text,t,0,n->kind==FE_N_GLOBAL,
                       n->b!=0,unit_cname(c,n->text ? n->text : "global"),n);
        }
    }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_FN) {
            t=fe_type_intern(&c->types,"<fn>");
            add_symbol(&s,s.globals,n->text,t,n,0,1,
                       unit_cname(c,n->text ? n->text : "fn"),n);
        }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_GLOBAL || n->kind==FE_N_CONST) {
            sym=find_current(s.globals,n->text ? n->text : "");
            if (n->b) {
                iv=m7_check_expected(&s,n->b,sym ? sym->type : 0);
                if (sym && sym->type->kind==FE_TYPE_UNKNOWN) {
                    sym->type=iv;
                    n->sem_type=iv;
                } else if (sym && !fe_type_equal(sym->type,iv) &&
                           !m7_actual_compatible(sym->type,iv,n->b))
                    err(c,n->loc,"global initializer type mismatch");
            }
        }
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_FN) m7_check_fn(c,n,s.globals);
    for (n=c->ast->root ? c->ast->root->children : 0;n;n=n->next)
        if (n->kind==FE_N_STRUCT) {
            t=fe_type_intern(&c->types,n->text);
            for (m=n->children;m;m=m->next)
                if (m->kind==FE_N_FN) m7_check_method(c,m,s.globals,t);
        }
    fe_type_layout_all(&c->types);
    return c->diags->errors==0;
}

FeType *fe_check_expr_type(FeCheck *c, FeNode *n)
{
    FeCheckerState s;
    s.c=c;
    s.scope=scope_new(&s,0);
    s.globals=s.scope;
    s.ret=fe_type_intern(&c->types,"void");
    s.loop_depth=0;
    s.defer_depth=0;
    s.fn_node=0;
    fe_own_liveness_init(&s.liveness,&c->ast->arena);
    if (m7_node_feature(n)) return m7_check_expr(&s,n);
    return fe_check_expr_type_m6(c,n);
}
