#ifndef FE_LOWER_H
#define FE_LOWER_H

#include "types.h"

#define FE_LOWER_NO_SCOPE ((unsigned)~0U)

typedef enum FeLowerExitKind {
    FE_LOWER_EXIT_FALLTHROUGH = 0,
    FE_LOWER_EXIT_RETURN,
    FE_LOWER_EXIT_ERROR_RETURN,
    FE_LOWER_EXIT_BREAK,
    FE_LOWER_EXIT_CONTINUE
} FeLowerExitKind;

typedef enum FeLowerCleanupKind {
    FE_LOWER_CLEANUP_DROP = 0,
    FE_LOWER_CLEANUP_DEFER
} FeLowerCleanupKind;

typedef struct FeLowerScope {
    FeNode *block;
    unsigned parent;
    unsigned ordinal;
} FeLowerScope;

typedef struct FeLowerCleanup {
    FeLowerCleanupKind kind;
    unsigned scope;
    unsigned ordinal;
    FeNode *node;
    FeNode *decl;
    FeType *type;
} FeLowerCleanup;

typedef struct FeLowerPlan {
    FeArena *arena;
    FeNode *fn;
    FeLowerScope *scopes;
    unsigned scope_count;
    unsigned scope_capacity;
    FeLowerCleanup *cleanups;
    unsigned cleanup_count;
    unsigned cleanup_capacity;
    unsigned next_ordinal;
} FeLowerPlan;

void fe_lower_plan_init(FeLowerPlan *plan, FeArena *arena);
int fe_lower_plan_build(FeLowerPlan *plan, FeNode *fn);
unsigned fe_lower_scope_for_block(const FeLowerPlan *plan,
                                  const FeNode *block);
unsigned fe_lower_collect_cleanups(const FeLowerPlan *plan,
                                   const FeNode *from_block,
                                   const FeNode *stop_block,
                                   const FeLowerCleanup **out,
                                   unsigned out_capacity);
int fe_lower_type_needs_drop(const FeType *type);
int fe_lower_exit_runs_cleanup(FeLowerExitKind kind);
int fe_lower_exit_leaves_function(FeLowerExitKind kind);

#endif
