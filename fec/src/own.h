#ifndef FE_OWN_H
#define FE_OWN_H

#include "types.h"
#include "diag.h"

#define FE_OWN_NODE_CONSUMED      0x100U
#define FE_OWN_NODE_DEFER_CAPTURE 0x200U

enum FeOwnMoveState {
    FE_OWN_AVAILABLE = 0,
    FE_OWN_MOVED = 1,
    FE_OWN_MAYBE_MOVED = 2
};

int fe_own_is_copy_type(FeType *type);
void fe_own_mark_consumed(FeDiags *diags, int *state, FeNode *decl,
                          FeNode *expr, FeType *type, int in_defer);
void fe_own_check_use(FeDiags *diags, int state, FeLoc loc);
int fe_own_merge_move(int left, int right);
int fe_own_loop_entry(int before, int after);
int fe_own_loop_exit(int state, int after);

#endif
