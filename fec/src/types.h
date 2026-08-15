#ifndef FE_TYPES_H
#define FE_TYPES_H

#include "ast.h"

typedef enum FeTypeKind {
    FE_TYPE_ERROR, FE_TYPE_VOID, FE_TYPE_BOOL, FE_TYPE_CHAR, FE_TYPE_INT,
    FE_TYPE_STRUCT, FE_TYPE_ENUM, FE_TYPE_ARRAY, FE_TYPE_SLICE, FE_TYPE_STR,
    FE_TYPE_REF, FE_TYPE_UNKNOWN
} FeTypeKind;

typedef struct FeFieldType FeFieldType;
typedef struct FeVariantType FeVariantType;

struct FeFieldType {
    char *name;
    FeType *type;
    unsigned long offset;
    const FeNode *ast_node;
};

struct FeVariantType {
    char *name;
    FeFieldType *fields;
    unsigned field_count;
    unsigned tag;
    const FeNode *ast_node;
    char *maker;
};

struct FeType {
    FeTypeKind kind;
    char name[64];
    char *cname;
    char *maker;
    char *indexer;
    char *slicer;
    char *full_slicer;
    char *tail_slicer;
    unsigned bits;
    int is_unsigned;
    int packed;
    unsigned long length;
    unsigned long size;
    unsigned align;
    FeType *elem;
    int ref_mut;
    FeFieldType *fields;
    unsigned field_count;
    FeVariantType *variants;
    unsigned variant_count;
    FeType *next;
    int emit_state;
    int cycle_state;
};

typedef struct FeTypeCtx {
    FeArena *arena;
    FeType *types;
    unsigned pointer_bits;
    const char *unit_name;
    unsigned generated_serial;
} FeTypeCtx;

void fe_types_init(FeTypeCtx *ctx, FeArena *arena, unsigned pointer_bits);
FeType *fe_type_intern(FeTypeCtx *ctx, const char *name);
FeType *fe_type_from_ast(FeTypeCtx *ctx, const FeNode *node);
FeType *fe_type_array(FeTypeCtx *ctx, unsigned long length, FeType *elem);
FeType *fe_type_slice(FeTypeCtx *ctx, FeType *elem);
FeType *fe_type_ref(FeTypeCtx *ctx, FeType *elem, int mutable);
FeType *fe_type_declare_struct(FeTypeCtx *ctx, const FeNode *node, int packed);
FeType *fe_type_declare_enum(FeTypeCtx *ctx, const FeNode *node);
void fe_type_layout_all(FeTypeCtx *ctx);
FeFieldType *fe_type_field(FeType *t, const char *name);
FeVariantType *fe_type_variant(FeType *t, const char *name);
int fe_type_equal(const FeType *a, const FeType *b);
int fe_type_is_integer(const FeType *t);
int fe_type_is_indexable(const FeType *t);
const char *fe_type_c_name(const FeType *t, unsigned pointer_bits);
unsigned long fe_type_size(const FeType *t);
unsigned fe_type_align(const FeType *t);

#endif
