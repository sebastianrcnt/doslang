#ifndef FE_TYPES_H
#define FE_TYPES_H

#include "ast.h"

typedef enum FeTypeKind {
    FE_TYPE_ERROR, FE_TYPE_VOID, FE_TYPE_BOOL, FE_TYPE_INT, FE_TYPE_UNKNOWN
} FeTypeKind;

struct FeType {
    FeTypeKind kind;
    char name[16];
    unsigned bits;
    int is_unsigned;
    FeType *next;
};

typedef struct FeTypeCtx {
    FeArena *arena;
    FeType *types;
    unsigned pointer_bits;
} FeTypeCtx;

void fe_types_init(FeTypeCtx *ctx, FeArena *arena, unsigned pointer_bits);
FeType *fe_type_intern(FeTypeCtx *ctx, const char *name);
FeType *fe_type_from_ast(FeTypeCtx *ctx, const FeNode *node);
int fe_type_equal(const FeType *a, const FeType *b);
int fe_type_is_integer(const FeType *t);
const char *fe_type_c_name(const FeType *t, unsigned pointer_bits);

#endif
