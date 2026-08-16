/* M7 extension layer for compiler A.  Keep the M6 implementation intact and
   replace only the two entry points whose semantics grow for wrapper types. */
#define fe_own_is_copy_type fe_own_is_copy_type_m6
#define fe_own_mark_consumed fe_own_mark_consumed_m6
#include "own.c"
#undef fe_own_is_copy_type
#undef fe_own_mark_consumed

static int m7_replace_unwrap(FeNode *expr)
{
    FeNode *call;
    FeNode *member;
    if (!expr || expr->kind != FE_N_MEMBER || !expr->text ||
        strcmp(expr->text,".?") != 0)
        return 0;
    call=expr->a;
    if (!call || call->kind != FE_N_CALL || !call->a ||
        call->a->kind != FE_N_MEMBER)
        return 0;
    member=call->a;
    return member->a && member->a->kind==FE_N_IDENT && member->a->text &&
           strcmp(member->a->text,"mem")==0 && member->b && member->b->text &&
           strcmp(member->b->text,"replace")==0;
}

int fe_own_is_copy_type(FeType *type)
{
    if (!type) return 1;
    if (type->kind==FE_TYPE_OPTIONAL)
        return fe_own_is_copy_type(type->elem);
    if (type->kind==FE_TYPE_ERROR_UNION)
        return fe_own_is_copy_type(type->error_value);
    return fe_own_is_copy_type_m6(type);
}

void fe_own_mark_consumed(FeDiags *diags, int *state, FeNode *decl,
                          FeNode *expr, FeType *type, int in_defer)
{
    if (!expr || !type || fe_own_is_copy_type(type)) return;
    if (expr->kind == FE_N_INDEX && type->kind == FE_TYPE_SLICE &&
        (expr->c || !expr->b))
        return;
    if ((expr->kind == FE_N_MEMBER || expr->kind == FE_N_INDEX) &&
        !m7_replace_unwrap(expr)) {
        fe_diag_error(diags, expr->loc,
            "cannot move a non-Copy value out of a projection; use mem.replace");
        return;
    }
    if (expr->kind != FE_N_IDENT || !state) return;
    if (in_defer) {
        if (decl) decl->flags |= FE_OWN_NODE_DEFER_CAPTURE;
        return;
    }
    *state = FE_OWN_MOVED;
    expr->flags |= FE_OWN_NODE_CONSUMED;
}
