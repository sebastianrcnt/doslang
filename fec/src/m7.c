#include "m7.h"
#include <stdio.h>
#include <string.h>

static char *m7_generated_name(FeTypeCtx *ctx, const char *prefix)
{
    char number[24];
    char *p;
    unsigned long n;
    sprintf(number, "%u", ctx->generated_serial++);
    n = (unsigned long)strlen(prefix) + (unsigned long)strlen(number) + 1UL;
    p = (char *)fe_arena_alloc(ctx->arena, n);
    if (!p) return 0;
    strcpy(p, prefix);
    strcat(p, number);
    return p;
}

int fe_m7_optional_uses_niche(const FeType *payload)
{
    if (!payload) return 0;
    if (payload->kind == FE_TYPE_REF) return 1;
    if (payload->kind == FE_TYPE_OWNED &&
        !(payload->elem && payload->elem->kind == FE_TYPE_SLICE))
        return 1;
    return 0;
}

FeType *fe_m7_optional_type(FeTypeCtx *ctx, FeType *payload)
{
    char key[128];
    FeType *t;
    if (!ctx || !payload) return 0;
    sprintf(key, "?%s", payload->name);
    t = fe_type_intern(ctx, key);
    if (!t) return 0;
    if (t->kind == FE_TYPE_UNKNOWN) {
        t->kind = FE_TYPE_OPTIONAL;
        t->elem = payload;
        if (!fe_m7_optional_uses_niche(payload)) {
            t->cname = m7_generated_name(ctx, "struct fe_option_");
            t->maker = m7_generated_name(ctx, "fe_make_option_");
        }
    }
    return t;
}

int fe_m7_can_contextual_null(const FeType *expected)
{
    return expected && expected->kind == FE_TYPE_OPTIONAL;
}

FeType *fe_m7_error_union_type(FeTypeCtx *ctx, FeType *error_type,
                               FeType *value_type)
{
    char key[160];
    FeType *t;
    if (!ctx || !value_type) return 0;
    if (!error_type || strcmp(error_type->name, "core.Error") == 0)
        return fe_type_error_union(ctx, value_type);
    sprintf(key, "%s!%s", error_type->name, value_type->name);
    t = fe_type_intern(ctx, key);
    if (!t) return 0;
    if (t->kind == FE_TYPE_UNKNOWN) {
        t->kind = FE_TYPE_ERROR_UNION;
        /* For FE_TYPE_ERROR_UNION, elem is the nominal error identity.
           A null elem denotes the built-in core.Error shorthand !T. */
        t->elem = error_type;
        t->error_value = value_type;
        if (value_type->kind != FE_TYPE_VOID) {
            t->cname = m7_generated_name(ctx, "struct fe_result_");
            t->maker = m7_generated_name(ctx, "fe_make_result_");
            t->alloc_cname = m7_generated_name(ctx, "fe_alloc_result_");
        }
    }
    return t;
}

FeType *fe_m7_error_type(FeTypeCtx *ctx, const FeType *error_union)
{
    if (!ctx || !error_union || error_union->kind != FE_TYPE_ERROR_UNION)
        return 0;
    if (error_union->elem) return error_union->elem;
    return fe_type_intern(ctx, "core.Error");
}

FeM7ContextKind fe_m7_error_context(FeTypeCtx *ctx, const FeType *expected,
                                    const FeType *actual)
{
    FeType *error_type;
    if (!ctx || !expected || !actual ||
        expected->kind != FE_TYPE_ERROR_UNION)
        return FE_M7_CONTEXT_NONE;
    if (expected->error_value && fe_type_equal(expected->error_value, actual))
        return FE_M7_CONTEXT_SUCCESS;
    error_type = fe_m7_error_type(ctx, expected);
    if (error_type && fe_type_equal(error_type, actual))
        return FE_M7_CONTEXT_FAILURE;
    return FE_M7_CONTEXT_NONE;
}

FeM7LazyKind fe_m7_lazy_kind(const FeNode *node)
{
    if (!node || !node->text) return FE_M7_LAZY_NONE;
    if (strcmp(node->text, "orelse") == 0) return FE_M7_LAZY_ORELSE;
    if (strcmp(node->text, "catch") == 0) return FE_M7_LAZY_CATCH;
    return FE_M7_LAZY_NONE;
}

int fe_m7_is_try(const FeNode *node)
{
    return node && node->kind == FE_N_UNARY && node->text &&
           strcmp(node->text, "try") == 0;
}
