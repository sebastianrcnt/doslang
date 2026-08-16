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

typedef enum FeOwnAccessKind {
    FE_OWN_READ,
    FE_OWN_WRITE,
    FE_OWN_MOVE,
    FE_OWN_BORROW_SHARED,
    FE_OWN_BORROW_MUT,
    FE_OWN_PROJECTION
} FeOwnAccessKind;

typedef enum FeOwnProvenanceKind {
    FE_OWN_PROV_INVALID,
    FE_OWN_PROV_STATIC,
    FE_OWN_PROV_PARAM
} FeOwnProvenanceKind;

typedef struct FeOwnPlace {
    FeNode *root;
    const char *root_cname;
    int projected;
} FeOwnPlace;

typedef struct FeOwnState {
    int move;
    int initialized;
    unsigned shared;
    int exclusive;
    int borrow_conflict;
    FeLoc move_loc;
    FeLoc borrow_loc;
} FeOwnState;

typedef struct FeOwnProvenance {
    FeOwnProvenanceKind kind;
    unsigned param_index;
} FeOwnProvenance;

typedef struct FeOwnLastUse {
    const char *cname;
    FeNode *decl;
    FeNode *last_node;
    unsigned long last_ordinal;
    int defer_extended;
} FeOwnLastUse;

typedef struct FeOwnLiveness {
    FeArena *arena;
    FeOwnLastUse *items;
    unsigned count;
    unsigned capacity;
    unsigned long ordinal;
} FeOwnLiveness;

int fe_own_is_copy_type(FeType *type);
int fe_own_is_reference_like(FeType *type);
int fe_own_place_from_expr(FeNode *expr, FeOwnPlace *place);

void fe_own_state_init(FeOwnState *state, int initialized);
int fe_own_access(FeDiags *diags, FeOwnState *state,
                  FeOwnAccessKind access, FeLoc loc);
void fe_own_release_shared(FeOwnState *state);
void fe_own_release_exclusive(FeOwnState *state);
FeOwnState fe_own_merge_state(FeOwnState left, FeOwnState right);
int fe_own_state_equal(const FeOwnState *left, const FeOwnState *right);

FeOwnProvenance fe_own_provenance_static(void);
FeOwnProvenance fe_own_provenance_param(unsigned param_index);
FeOwnProvenance fe_own_merge_provenance(FeOwnProvenance left,
                                        FeOwnProvenance right);

void fe_own_liveness_init(FeOwnLiveness *live, FeArena *arena);
int fe_own_collect_last_uses(FeOwnLiveness *live, FeNode *fn);
const FeOwnLastUse *fe_own_last_use(const FeOwnLiveness *live,
                                    const char *cname);

void fe_own_mark_consumed(FeDiags *diags, int *state, FeNode *decl,
                          FeNode *expr, FeType *type, int in_defer);
void fe_own_check_use(FeDiags *diags, int state, FeLoc loc);
int fe_own_merge_move(int left, int right);
int fe_own_loop_entry(int before, int after);
int fe_own_loop_exit(int state, int after);

#endif
