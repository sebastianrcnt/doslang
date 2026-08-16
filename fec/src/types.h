#ifndef FE_TYPES_H
#define FE_TYPES_H

#include "ast.h"

typedef enum FeTypeKind {
    FE_TYPE_ERROR, FE_TYPE_ERROR_UNION, FE_TYPE_OPTIONAL,
    FE_TYPE_VOID, FE_TYPE_BOOL, FE_TYPE_CHAR, FE_TYPE_INT,
    FE_TYPE_STRUCT, FE_TYPE_ENUM, FE_TYPE_ARRAY, FE_TYPE_SLICE, FE_TYPE_STR,
    FE_TYPE_REF, FE_TYPE_OWNED, FE_TYPE_UNKNOWN
} FeTypeKind;

typedef struct FeFieldType FeFieldType;

/* A type parameter bound to an argument while an instance is checked. */
#define FE_TYPE_PARAM_MAX 8
typedef struct FeTypeBind {
    const char *name;
    FeType *type;
} FeTypeBind;

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
    /* Long enough for a nested instance spelling such as
       `Box(Box(Box(i32)))` at the depth limit. */
    char name[256];
    /* The unit that declared this type, for the nominal kinds. NULL for
       builtins and for structural types like `[]u8`, which every unit
       shares. Two units declaring the same name declare two types. */
    const char *unit;
    char *cname;
    char *maker;
    char *none_cname;
    char *unwrap_cname;
    char *indexer;
    char *slicer;
    char *full_slicer;
    char *tail_slicer;
    char *drop_cname;
    char *alloc_cname;
    char *replace_cname;
    unsigned bits;
    int is_unsigned;
    int packed;
    int is_error;
    int has_drop;
    unsigned long length;
    unsigned long size;
    unsigned align;
    /* Element/payload type for refs, owners, slices and optionals.  For an
       error union this is the nominal error identity; NULL means core.Error. */
    FeType *elem;
    /* Success value for an error union. */
    FeType *error_value;
    int ref_mut;
    FeFieldType *fields;
    unsigned field_count;
    FeVariantType *variants;
    unsigned variant_count;
    /* The declaration this type came from, and the bindings that made it if
       it is a generic instance. A method has to be checked with the same
       bindings the instance was built with. */
    /* A small unique number, used to name an instance whose readable
       spelling would be too long to keep distinct. */
    unsigned serial;
    const FeNode *decl_node;
    FeTypeBind binds[FE_TYPE_PARAM_MAX];
    unsigned bind_count;
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
    /* Bindings in force right now. A name that is a bound parameter is
       that argument's type and nothing else. */
    FeTypeBind params[FE_TYPE_PARAM_MAX];
    unsigned param_count;
    /* Instantiate `Name(args...)`. Only the checker knows the declarations,
       so it installs this and the type layer calls back into it. */
    FeType *(*instantiate)(void *owner, const FeNode *node);
    void *instantiate_owner;
} FeTypeCtx;

void fe_types_init(FeTypeCtx *ctx, FeArena *arena, unsigned pointer_bits);
FeType *fe_type_intern(FeTypeCtx *ctx, const char *name);
/* Intern a nominal type belonging to `unit` rather than to whichever unit
   is being checked. Used to name a type across a unit boundary. */
FeType *fe_type_intern_unit(FeTypeCtx *ctx, const char *unit,
                            const char *name);
FeType *fe_type_from_ast(FeTypeCtx *ctx, const FeNode *node);
FeType *fe_type_array(FeTypeCtx *ctx, unsigned long length, FeType *elem);
FeType *fe_type_slice(FeTypeCtx *ctx, FeType *elem);
FeType *fe_type_mut_slice(FeTypeCtx *ctx, FeType *elem);
FeType *fe_type_ref(FeTypeCtx *ctx, FeType *elem, int mutable);
FeType *fe_type_owned(FeTypeCtx *ctx, FeType *elem);
FeType *fe_type_error_union(FeTypeCtx *ctx, FeType *value);
void fe_type_require_replace(FeTypeCtx *ctx, FeType *type);
FeType *fe_type_declare_struct(FeTypeCtx *ctx, const FeNode *node, int packed);
FeType *fe_type_declare_enum(FeTypeCtx *ctx, const FeNode *node);
FeType *fe_type_declare_error(FeTypeCtx *ctx, const FeNode *node);
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
