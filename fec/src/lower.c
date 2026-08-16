#include "lower.h"
#include <string.h>

static int lower_grow_scopes(FeLowerPlan *plan)
{
    FeLowerScope *items;
    unsigned capacity;
    if (plan->scope_count < plan->scope_capacity) return 1;
    capacity = plan->scope_capacity ? plan->scope_capacity * 2U : 8U;
    items = (FeLowerScope *)fe_arena_alloc(plan->arena,
                                            capacity * sizeof(FeLowerScope));
    if (!items) return 0;
    if (plan->scopes)
        memcpy(items, plan->scopes,
               plan->scope_count * sizeof(FeLowerScope));
    plan->scopes = items;
    plan->scope_capacity = capacity;
    return 1;
}

static int lower_grow_cleanups(FeLowerPlan *plan)
{
    FeLowerCleanup *items;
    unsigned capacity;
    if (plan->cleanup_count < plan->cleanup_capacity) return 1;
    capacity = plan->cleanup_capacity ? plan->cleanup_capacity * 2U : 16U;
    items = (FeLowerCleanup *)fe_arena_alloc(plan->arena,
                                              capacity * sizeof(FeLowerCleanup));
    if (!items) return 0;
    if (plan->cleanups)
        memcpy(items, plan->cleanups,
               plan->cleanup_count * sizeof(FeLowerCleanup));
    plan->cleanups = items;
    plan->cleanup_capacity = capacity;
    return 1;
}

static unsigned lower_add_scope(FeLowerPlan *plan, FeNode *block,
                                unsigned parent)
{
    FeLowerScope *scope;
    unsigned index;
    if (!lower_grow_scopes(plan)) return FE_LOWER_NO_SCOPE;
    index = plan->scope_count++;
    scope = &plan->scopes[index];
    scope->block = block;
    scope->parent = parent;
    scope->ordinal = plan->next_ordinal++;
    return index;
}

static int lower_add_cleanup(FeLowerPlan *plan, unsigned scope,
                             FeLowerCleanupKind kind, FeNode *node,
                             FeNode *decl, FeType *type)
{
    FeLowerCleanup *cleanup;
    if (!lower_grow_cleanups(plan)) return 0;
    cleanup = &plan->cleanups[plan->cleanup_count++];
    cleanup->kind = kind;
    cleanup->scope = scope;
    cleanup->ordinal = plan->next_ordinal++;
    cleanup->node = node;
    cleanup->decl = decl;
    cleanup->type = type;
    return 1;
}

int fe_lower_type_needs_drop(const FeType *type)
{
    unsigned i;
    unsigned j;
    if (!type) return 0;
    if (type->kind == FE_TYPE_OWNED) return 1;
    if (type->kind == FE_TYPE_OPTIONAL)
        return fe_lower_type_needs_drop(type->elem);
    if (type->kind == FE_TYPE_ERROR_UNION)
        return fe_lower_type_needs_drop(type->error_value);
    if (type->kind == FE_TYPE_ARRAY)
        return fe_lower_type_needs_drop(type->elem);
    if (type->kind == FE_TYPE_STRUCT) {
        if (type->has_drop) return 1;
        for (i = 0; i < type->field_count; ++i)
            if (fe_lower_type_needs_drop(type->fields[i].type)) return 1;
        return 0;
    }
    if (type->kind == FE_TYPE_ENUM) {
        for (i = 0; i < type->variant_count; ++i)
            for (j = 0; j < type->variants[i].field_count; ++j)
                if (fe_lower_type_needs_drop(type->variants[i].fields[j].type))
                    return 1;
    }
    return 0;
}

static int lower_build_node(FeLowerPlan *plan, FeNode *node,
                            unsigned scope);

static int lower_build_list(FeLowerPlan *plan, FeNode *node,
                            unsigned scope)
{
    while (node) {
        if (!lower_build_node(plan, node, scope)) return 0;
        node = node->next;
    }
    return 1;
}

static int lower_build_block(FeLowerPlan *plan, FeNode *block,
                             unsigned parent)
{
    unsigned scope;
    if (!block || block->kind != FE_N_BLOCK) return 1;
    scope = lower_add_scope(plan, block, parent);
    if (scope == FE_LOWER_NO_SCOPE) return 0;
    return lower_build_list(plan, block->children, scope);
}

static int lower_build_node(FeLowerPlan *plan, FeNode *node,
                            unsigned scope)
{
    FeNode *child;
    FeType *type;
    if (!node) return 1;
    if (node->kind == FE_N_BLOCK)
        return lower_build_block(plan, node, scope);
    if (node->kind == FE_N_DEFER) {
        if (!lower_add_cleanup(plan, scope, FE_LOWER_CLEANUP_DEFER,
                               node->a, node, 0))
            return 0;
        return lower_build_node(plan, node->a, scope);
    }
    if (node->kind == FE_N_LET || node->kind == FE_N_VAR ||
        node->kind == FE_N_CONST) {
        type = node->sem_type;
        if (fe_lower_type_needs_drop(type))
            if (!lower_add_cleanup(plan, scope, FE_LOWER_CLEANUP_DROP,
                                   node, node, type))
                return 0;
    }
    if (!lower_build_node(plan, node->a, scope)) return 0;
    if (!lower_build_node(plan, node->b, scope)) return 0;
    if (!lower_build_node(plan, node->c, scope)) return 0;
    for (child = node->children; child; child = child->next)
        if (!lower_build_node(plan, child, scope)) return 0;
    return 1;
}

void fe_lower_plan_init(FeLowerPlan *plan, FeArena *arena)
{
    if (!plan) return;
    plan->arena = arena;
    plan->fn = 0;
    plan->scopes = 0;
    plan->scope_count = 0;
    plan->scope_capacity = 0;
    plan->cleanups = 0;
    plan->cleanup_count = 0;
    plan->cleanup_capacity = 0;
    plan->next_ordinal = 0;
}

int fe_lower_plan_build(FeLowerPlan *plan, FeNode *fn)
{
    if (!plan || !plan->arena || !fn || fn->kind != FE_N_FN) return 0;
    plan->fn = fn;
    plan->scopes = 0;
    plan->scope_count = 0;
    plan->scope_capacity = 0;
    plan->cleanups = 0;
    plan->cleanup_count = 0;
    plan->cleanup_capacity = 0;
    plan->next_ordinal = 0;
    if (!fn->c) return 1;
    return lower_build_block(plan, fn->c, FE_LOWER_NO_SCOPE);
}

unsigned fe_lower_scope_for_block(const FeLowerPlan *plan,
                                  const FeNode *block)
{
    unsigned i;
    if (!plan || !block) return FE_LOWER_NO_SCOPE;
    for (i = 0; i < plan->scope_count; ++i)
        if (plan->scopes[i].block == block) return i;
    return FE_LOWER_NO_SCOPE;
}

unsigned fe_lower_collect_cleanups(const FeLowerPlan *plan,
                                   const FeNode *from_block,
                                   const FeNode *stop_block,
                                   const FeLowerCleanup **out,
                                   unsigned out_capacity)
{
    unsigned scope;
    unsigned stop;
    unsigned i;
    unsigned count;
    if (!plan || !from_block) return 0;
    scope = fe_lower_scope_for_block(plan, from_block);
    stop = stop_block ? fe_lower_scope_for_block(plan, stop_block) :
                        FE_LOWER_NO_SCOPE;
    count = 0;
    while (scope != FE_LOWER_NO_SCOPE && scope != stop) {
        for (i = plan->cleanup_count; i > 0; --i) {
            if (plan->cleanups[i - 1U].scope != scope) continue;
            if (out && count < out_capacity)
                out[count] = &plan->cleanups[i - 1U];
            ++count;
        }
        scope = plan->scopes[scope].parent;
    }
    return count;
}

int fe_lower_exit_runs_cleanup(FeLowerExitKind kind)
{
    return kind == FE_LOWER_EXIT_FALLTHROUGH ||
           kind == FE_LOWER_EXIT_RETURN ||
           kind == FE_LOWER_EXIT_ERROR_RETURN ||
           kind == FE_LOWER_EXIT_BREAK ||
           kind == FE_LOWER_EXIT_CONTINUE;
}

int fe_lower_exit_leaves_function(FeLowerExitKind kind)
{
    return kind == FE_LOWER_EXIT_RETURN ||
           kind == FE_LOWER_EXIT_ERROR_RETURN;
}
