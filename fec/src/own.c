#include "own.h"

int fe_own_is_copy_type(FeType *type)
{
    unsigned i;
    if (!type) return 1;
    if (type->kind == FE_TYPE_OWNED) return 0;
    if (type->kind == FE_TYPE_REF || type->kind == FE_TYPE_SLICE)
        return !type->ref_mut;
    if (type->kind == FE_TYPE_ARRAY)
        return fe_own_is_copy_type(type->elem);
    if (type->kind == FE_TYPE_STRUCT) {
        if (type->has_drop) return 0;
        for (i = 0; i < type->field_count; ++i)
            if (!fe_own_is_copy_type(type->fields[i].type)) return 0;
    }
    if (type->kind == FE_TYPE_ENUM) {
        for (i = 0; i < type->variant_count; ++i) {
            unsigned j;
            for (j = 0; j < type->variants[i].field_count; ++j)
                if (!fe_own_is_copy_type(type->variants[i].fields[j].type))
                    return 0;
        }
    }
    return 1;
}

void fe_own_mark_consumed(FeDiags *diags, int *state, FeNode *decl,
                          FeNode *expr, FeType *type, int in_defer)
{
    if (!expr || !type || fe_own_is_copy_type(type)) return;
    if (expr->kind == FE_N_INDEX && type->kind == FE_TYPE_SLICE &&
        (expr->c || !expr->b))
        return;
    if (expr->kind == FE_N_MEMBER || expr->kind == FE_N_INDEX) {
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
    /* The consuming use owns the live-flag transition.  The declaration must
       stay live on control-flow paths where the move did not execute. */
    expr->flags |= FE_OWN_NODE_CONSUMED;
}

void fe_own_check_use(FeDiags *diags, int state, FeLoc loc)
{
    if (state == FE_OWN_MOVED)
        fe_diag_error(diags, loc, "use of moved value");
    else if (state == FE_OWN_MAYBE_MOVED)
        fe_diag_error(diags, loc, "use of possibly moved value");
}

int fe_own_merge_move(int left, int right)
{
    if (left == FE_OWN_MOVED && right == FE_OWN_MOVED)
        return FE_OWN_MOVED;
    if (left != FE_OWN_AVAILABLE || right != FE_OWN_AVAILABLE)
        return FE_OWN_MAYBE_MOVED;
    return FE_OWN_AVAILABLE;
}

int fe_own_loop_entry(int before, int after)
{
    if (before == after) return before;
    return FE_OWN_MAYBE_MOVED;
}

int fe_own_loop_exit(int state, int after)
{
    if (after != FE_OWN_AVAILABLE) return FE_OWN_MAYBE_MOVED;
    return state;
}
