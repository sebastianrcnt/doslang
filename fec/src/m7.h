#ifndef FE_M7_H
#define FE_M7_H

#include "types.h"

typedef enum FeM7ContextKind {
    FE_M7_CONTEXT_NONE = 0,
    FE_M7_CONTEXT_SUCCESS,
    FE_M7_CONTEXT_FAILURE
} FeM7ContextKind;

typedef enum FeM7LazyKind {
    FE_M7_LAZY_NONE = 0,
    FE_M7_LAZY_ORELSE,
    FE_M7_LAZY_CATCH
} FeM7LazyKind;

FeType *fe_m7_optional_type(FeTypeCtx *ctx, FeType *payload);
int fe_m7_optional_uses_niche(const FeType *payload);
int fe_m7_can_contextual_null(const FeType *expected);

FeType *fe_m7_error_union_type(FeTypeCtx *ctx, FeType *error_type,
                               FeType *value_type);
FeType *fe_m7_error_type(FeTypeCtx *ctx, const FeType *error_union);
FeM7ContextKind fe_m7_error_context(FeTypeCtx *ctx, const FeType *expected,
                                    const FeType *actual);

FeM7LazyKind fe_m7_lazy_kind(const FeNode *node);
int fe_m7_is_try(const FeNode *node);

#endif
