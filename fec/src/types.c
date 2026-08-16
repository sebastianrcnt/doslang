#include "types.h"
#include "m7.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static FeType *new_type(FeTypeCtx *ctx, const char *name, FeTypeKind kind)
{
    FeType *t;
    unsigned i;
    t = (FeType *)fe_arena_alloc(ctx->arena, sizeof(FeType));
    if (!t) return 0;
    for (i = 0; i + 1U < sizeof(t->name) && name && name[i]; ++i)
        t->name[i] = name[i];
    t->name[i] = '\0';
    t->kind = kind;
    t->unit = 0;
    t->cname = 0;
    t->maker = 0;
    t->none_cname = 0;
    t->unwrap_cname = 0;
    t->indexer = 0;
    t->slicer = 0;
    t->full_slicer = 0;
    t->tail_slicer = 0;
    t->drop_cname = 0;
    t->alloc_cname = 0;
    t->replace_cname = 0;
    t->bits = 0;
    t->is_unsigned = 0;
    t->packed = 0;
    t->is_error = 0;
    t->has_drop = 0;
    t->length = 0;
    t->size = 0;
    t->align = 1;
    t->elem = 0;
    t->error_value = 0;
    t->ref_mut = 0;
    t->fields = 0;
    t->field_count = 0;
    t->variants = 0;
    t->variant_count = 0;
    t->serial = ctx->generated_serial++;
    t->decl_node = 0;
    t->bind_count = 0;
    t->next = ctx->types;
    t->emit_state = 0;
    t->cycle_state = 0;
    ctx->types = t;
    return t;
}

void fe_types_init(FeTypeCtx *ctx, FeArena *arena, unsigned pointer_bits)
{
    ctx->arena = arena;
    ctx->types = 0;
    ctx->pointer_bits = pointer_bits;
    ctx->unit_name = "unit";
    ctx->generated_serial = 0;
    ctx->param_count = 0;
    ctx->instantiate = 0;
    ctx->instantiate_owner = 0;
}

/* Does this type answer to `name` for someone checking `unit`? A type with no
   unit is shared by everyone; one with a unit answers only inside it. */
static int type_visible_as(const FeType *t, const char *unit, const char *name)
{
    if (strcmp(t->name, name) != 0) return 0;
    if (!t->unit) return 1;
    return unit && strcmp(t->unit, unit) == 0;
}

FeType *fe_type_intern_unit(FeTypeCtx *ctx, const char *unit, const char *name)
{
    FeType *t;
    if (!name) name = "<unknown>";
    if (!unit) return fe_type_intern(ctx, name);
    for (t = ctx->types; t; t = t->next)
        if (t->unit && strcmp(t->name, name) == 0 &&
            strcmp(t->unit, unit) == 0) return t;
    t = new_type(ctx, name, FE_TYPE_UNKNOWN);
    if (t) t->unit = unit;
    return t;
}

FeType *fe_type_intern(FeTypeCtx *ctx, const char *name)
{
    FeType *t;
    unsigned bits = 0;
    int uns = 0;
    FeTypeKind kind = FE_TYPE_UNKNOWN;
    unsigned i;
    if (!name) name = "<unknown>";
    /* A bound type parameter is its argument, and shadows everything. */
    for (i = 0; i < ctx->param_count; ++i)
        if (strcmp(ctx->params[i].name, name) == 0) return ctx->params[i].type;
    for (t = ctx->types; t; t = t->next)
        if (type_visible_as(t, ctx->unit_name, name)) return t;
    if (strcmp(name, "void") == 0) kind = FE_TYPE_VOID;
    else if (strcmp(name, "bool") == 0) kind = FE_TYPE_BOOL;
    else if (strcmp(name, "char") == 0) kind = FE_TYPE_CHAR;
    else if (strcmp(name, "str") == 0)
        return fe_type_slice(ctx, fe_type_intern(ctx, "u8"));
    else if (strcmp(name, "io.Writer") == 0) {
        kind = FE_TYPE_STRUCT;
    }
    /* The default error set. Its members are collected across the build rather
       than declared, so it carries an identity but no variant list. */
    else if (strcmp(name, "core.Error") == 0) {
        kind = FE_TYPE_ENUM; bits = 16; uns = 1;
    }
    else if (strcmp(name, "i8") == 0 || strcmp(name, "u8") == 0) {
        kind = FE_TYPE_INT; bits = 8; uns = name[0] == 'u';
    } else if (strcmp(name, "i16") == 0 || strcmp(name, "u16") == 0) {
        kind = FE_TYPE_INT; bits = 16; uns = name[0] == 'u';
    } else if (strcmp(name, "i32") == 0 || strcmp(name, "u32") == 0) {
        kind = FE_TYPE_INT; bits = 32; uns = name[0] == 'u';
    } else if (strcmp(name, "usize") == 0 || strcmp(name, "isize") == 0) {
        kind = FE_TYPE_INT; bits = ctx->pointer_bits; uns = name[0] == 'u';
    }
    t = new_type(ctx, name, kind);
    if (!t) return 0;
    t->bits = bits;
    t->is_unsigned = uns;
    if (strcmp(name,"core.Error")==0) { t->is_error = 1; t->size = 2; t->align = 2; }
    if (strcmp(name,"io.Writer")==0) {
        t->cname=fe_arena_strdup(ctx->arena,"fe_writer",10);
        t->size=4;
        t->align=1;
        return t;
    }
    if (kind == FE_TYPE_STR) {
        t->cname = fe_arena_strdup(ctx->arena, "fe_str", 6);
        t->elem = fe_type_intern(ctx, "u8");
        t->indexer = "fe_idx_str";
        t->slicer = "fe_slice_str";
        t->full_slicer = "fe_full_slice_str";
        t->tail_slicer = "fe_tail_slice_str";
    }
    return t;
}

static char *generated_name(FeTypeCtx *ctx, const char *prefix,
                            const char *name)
{
    char number[24];
    unsigned long n;
    char *p;
    sprintf(number, "%u", ctx->generated_serial++);
    n = (unsigned long)strlen(prefix) + (unsigned long)strlen(name) +
        (unsigned long)strlen(number) + 2UL;
    p = (char *)fe_arena_alloc(ctx->arena, n);
    if (!p) return 0;
    strcpy(p, prefix);
    strcat(p, name);
    strcat(p, "_");
    strcat(p, number);
    return p;
}

FeType *fe_type_array(FeTypeCtx *ctx, unsigned long length, FeType *elem)
{
    char key[320];
    FeType *t;
    sprintf(key, "[%lu]%s", length, elem ? elem->name : "?");
    t = fe_type_intern(ctx, key);
    if (t->kind == FE_TYPE_UNKNOWN) {
        t->kind = FE_TYPE_ARRAY;
        t->length = length;
        t->elem = elem;
        t->cname = generated_name(ctx, "struct fe_arr_", "type");
        t->maker = generated_name(ctx, "fe_make_arr_", "type");
        t->drop_cname = generated_name(ctx, "fe_drop_arr_", "type");
        t->indexer = generated_name(ctx, "fe_idx_arr_", "type");
        t->slicer = generated_name(ctx, "fe_slice_arr_", "type");
        t->full_slicer = generated_name(ctx, "fe_full_arr_", "type");
        t->tail_slicer = generated_name(ctx, "fe_tail_arr_", "type");
    }
    return t;
}

FeType *fe_type_slice(FeTypeCtx *ctx, FeType *elem)
{
    char key[320];
    FeType *t;
    sprintf(key, "[]%s", elem ? elem->name : "?");
    t = fe_type_intern(ctx, key);
    if (t->kind == FE_TYPE_UNKNOWN) {
        t->kind = FE_TYPE_SLICE;
        t->elem = elem;
        t->cname = generated_name(ctx, "fe_slice_", "type");
        t->maker = generated_name(ctx, "fe_make_slice_", "type");
        t->indexer = generated_name(ctx, "fe_idx_slice_", "type");
        t->slicer = generated_name(ctx, "fe_slice_slice_", "type");
        t->full_slicer = generated_name(ctx, "fe_full_slice_", "type");
        t->tail_slicer = generated_name(ctx, "fe_tail_slice_", "type");
    }
    return t;
}

FeType *fe_type_mut_slice(FeTypeCtx *ctx, FeType *elem)
{
    char key[320];
    FeType *t;
    sprintf(key, "[]mut %s", elem ? elem->name : "?");
    t = fe_type_intern(ctx, key);
    if (t->kind == FE_TYPE_UNKNOWN) {
        t->kind = FE_TYPE_SLICE;
        t->elem = elem;
        t->ref_mut = 1;
        t->cname = generated_name(ctx, "fe_mut_slice_", "type");
        t->maker = generated_name(ctx, "fe_make_mut_slice_", "type");
        t->indexer = generated_name(ctx, "fe_idx_mut_slice_", "type");
        t->slicer = generated_name(ctx, "fe_slice_mut_slice_", "type");
        t->full_slicer = generated_name(ctx, "fe_full_mut_slice_", "type");
        t->tail_slicer = generated_name(ctx, "fe_tail_mut_slice_", "type");
    }
    return t;
}

FeType *fe_type_ref(FeTypeCtx *ctx, FeType *elem, int mutable)
{
    char key[320];
    FeType *t;
    sprintf(key,"%s%s",mutable ? "&mut " : "&",elem ? elem->name : "?");
    t=fe_type_intern(ctx,key);
    if(t->kind==FE_TYPE_UNKNOWN) {
        t->kind=FE_TYPE_REF;
        t->elem=elem;
        t->ref_mut=mutable;
    }
    return t;
}

FeType *fe_type_owned(FeTypeCtx *ctx, FeType *elem)
{
    char key[320];
    FeType *t;
    sprintf(key,"^%s",elem ? elem->name : "?");
    t=fe_type_intern(ctx,key);
    if(t->kind==FE_TYPE_UNKNOWN) {
        t->kind=FE_TYPE_OWNED;
        t->elem=elem;
        if(elem && elem->kind==FE_TYPE_SLICE) {
            t->cname=generated_name(ctx,"fe_owned_slice_","type");
            t->maker=generated_name(ctx,"fe_make_owned_slice_","type");
        }
    }
    return t;
}

FeType *fe_type_error_union(FeTypeCtx *ctx, FeType *value)
{
    char key[320];
    FeType *t;
    sprintf(key,"!%s",value ? value->name : "?");
    t=fe_type_intern(ctx,key);
    if(t->kind==FE_TYPE_UNKNOWN) {
        t->kind=FE_TYPE_ERROR_UNION;
        t->error_value=value;
        t->drop_cname=generated_name(ctx,"fe_drop_result_","value");
        if (value && value->kind != FE_TYPE_VOID) {
            t->cname=generated_name(ctx,"struct fe_result_","value");
            t->maker=generated_name(ctx,"fe_make_result_","value");
            t->none_cname=generated_name(ctx,"fe_fail_result_","value");
            t->alloc_cname=generated_name(ctx,"fe_alloc_result_","value");
        }
    }
    return t;
}

void fe_type_require_replace(FeTypeCtx *ctx, FeType *type)
{
    if(type && !type->replace_cname)
        type->replace_cname=generated_name(ctx,"fe_replace_","type");
}

FeType *fe_type_declare_struct(FeTypeCtx *ctx, const FeNode *node, int packed)
{
    FeType *t;
    FeNode *f;
    unsigned count = 0;
    unsigned i = 0;
    char *cname;
    if (!node || !node->text) return 0;
    t = fe_type_intern_unit(ctx, ctx->unit_name, node->text);
    if (t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_STRUCT) return t;
    if (t->kind == FE_TYPE_STRUCT) return t;
    t->kind = FE_TYPE_STRUCT;
    t->packed = packed;
    t->decl_node = node;
    for (f = node->children; f; f = f->next)
        if (f->kind==FE_N_FN && f->text && strcmp(f->text,"drop")==0)
            t->has_drop=1;
    cname = (char *)fe_arena_alloc(ctx->arena,
        (unsigned long)strlen("struct fe_") + strlen(ctx->unit_name) +
        strlen(node->text) + 2UL);
    if (!cname) return t;
    strcpy(cname, "struct fe_");
    strcat(cname, ctx->unit_name);
    strcat(cname, "_");
    strcat(cname, node->text);
    t->cname = cname;
    t->maker = generated_name(ctx, "fe_make_", node->text);
    t->drop_cname = generated_name(ctx, "fe_drop_", node->text);
    for (f = node->children; f; f = f->next)
        if (f->kind == FE_N_FIELD) ++count;
    t->field_count = count;
    if (count) {
        t->fields = (FeFieldType *)fe_arena_alloc(ctx->arena,
                                                  count * sizeof(FeFieldType));
        if (!t->fields) return t;
        for (f = node->children; f; f = f->next) if (f->kind == FE_N_FIELD) {
            t->fields[i].name = f->text;
            t->fields[i].type = 0;
            t->fields[i].offset = 0;
            t->fields[i].ast_node = f;
            ++i;
        }
    }
    return t;
}

FeType *fe_type_declare_enum(FeTypeCtx *ctx, const FeNode *node)
{
    FeType *t;
    FeNode *v;
    unsigned count = 0;
    unsigned i = 0;
    char *cname;
    if (!node || !node->text) return 0;
    t = fe_type_intern_unit(ctx, ctx->unit_name, node->text);
    if (t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_ENUM) return t;
    if (t->kind == FE_TYPE_ENUM) return t;
    t->kind = FE_TYPE_ENUM;
    t->decl_node = node;
    cname = (char *)fe_arena_alloc(ctx->arena,
        (unsigned long)strlen("struct fe_") + strlen(ctx->unit_name) +
        strlen(node->text) + 2UL);
    if (!cname) return t;
    strcpy(cname, "struct fe_");
    strcat(cname, ctx->unit_name);
    strcat(cname, "_");
    strcat(cname, node->text);
    t->cname = cname;
    for (v = node->children; v; v = v->next) ++count;
    t->variant_count = count;
    if (count) {
        t->variants = (FeVariantType *)fe_arena_alloc(ctx->arena,
                                      count * sizeof(FeVariantType));
        if (!t->variants) return t;
        for (v = node->children; v; v = v->next) {
            t->variants[i].name = v->text;
            t->variants[i].fields = 0;
            t->variants[i].field_count = 0;
            if (node->kind==FE_N_ERROR_DECL && v->a &&
                v->a->kind==FE_N_LITERAL && v->a->text)
                t->variants[i].tag=(unsigned)strtoul(v->a->text,0,0);
            else t->variants[i].tag = i;
            t->variants[i].ast_node = v;
            t->variants[i].maker = generated_name(ctx, "fe_make_variant_", v->text ? v->text : "variant");
            if (v->a && v->a->kind == FE_N_TYPE) {
                t->variants[i].field_count = 1;
                t->variants[i].fields = (FeFieldType *)fe_arena_alloc(ctx->arena, sizeof(FeFieldType));
                if (t->variants[i].fields) {
                    t->variants[i].fields[0].name = "value";
                    t->variants[i].fields[0].type = fe_type_from_ast(ctx, v->a);
                    t->variants[i].fields[0].offset = 0;
                    t->variants[i].fields[0].ast_node = v;
                }
            }
            if (!v->a) {
                FeNode *f;
                unsigned fc = 0;
                unsigned j = 0;
                for (f = v->children; f; f = f->next)
                    if (f->kind == FE_N_FIELD) ++fc;
                t->variants[i].field_count = fc;
                if (fc) {
                    t->variants[i].fields = (FeFieldType *)fe_arena_alloc(
                        ctx->arena, fc * sizeof(FeFieldType));
                    if (t->variants[i].fields) for (f = v->children; f; f=f->next)
                        if (f->kind == FE_N_FIELD) {
                            t->variants[i].fields[j].name = f->text;
                            t->variants[i].fields[j].type = 0;
                            t->variants[i].fields[j].offset = 0;
                            t->variants[i].fields[j].ast_node = f;
                            ++j;
                        }
                }
            }
            ++i;
        }
    }
    return t;
}

FeType *fe_type_declare_error(FeTypeCtx *ctx, const FeNode *node)
{
    FeType *t=fe_type_declare_enum(ctx,node);
    if (t) t->is_error=1;
    return t;
}

static unsigned long round_up(unsigned long x, unsigned a)
{
    unsigned long rem;
    if (a <= 1U) return x;
    rem = x % (unsigned long)a;
    return rem ? x + (unsigned long)a - rem : x;
}

unsigned long fe_type_size(const FeType *t)
{
    return t ? t->size : 0;
}

unsigned fe_type_align(const FeType *t)
{
    return t && t->align ? t->align : 1U;
}

static void layout_type(FeTypeCtx *ctx, FeType *t)
{
    unsigned i;
    unsigned align;
    unsigned long off;
    unsigned long max_size;
    unsigned max_align;
    if (!t || t->size) return;
    if (t->cycle_state == 1) {
        t->size = 1;
        t->align = 1;
        return;
    }
    t->cycle_state = 1;
    if (t->kind == FE_TYPE_VOID || t->kind == FE_TYPE_UNKNOWN ||
        t->kind == FE_TYPE_ERROR) { t->size = 0; t->align = 1; t->cycle_state = 2; return; }
    if (t->kind == FE_TYPE_ERROR_UNION) {
        if (t->error_value && t->error_value->kind != FE_TYPE_VOID) {
            layout_type(ctx,t->error_value);
            t->align=fe_type_align(t->error_value);
            t->size=round_up(2UL,t->align)+fe_type_size(t->error_value);
            t->size=round_up(t->size,t->align);
        } else {
            t->size=2;
            t->align=2U;
        }
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_OPTIONAL) {
        layout_type(ctx,t->elem);
        if (fe_m7_optional_uses_niche(t->elem)) {
            t->size=fe_type_size(t->elem);
            t->align=fe_type_align(t->elem);
        } else {
            t->align=fe_type_align(t->elem);
            t->size=round_up(1UL,t->align)+fe_type_size(t->elem);
            t->size=round_up(t->size,t->align);
        }
        t->cycle_state=2; return;
    }
    if (t->kind == FE_TYPE_BOOL || t->kind == FE_TYPE_CHAR) {
        t->size = 1; t->align = 1; t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_INT) {
        t->size = (t->bits + 7U) / 8U;
        t->align = t->size;
        if (t->size > 4UL) t->size = 4UL;
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_REF) {
        t->size = FE_PTR_SIZE;
        t->align = FE_PTR_ALIGN;
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_OWNED) {
        t->size = t->elem && t->elem->kind==FE_TYPE_SLICE ?
            2UL * FE_PTR_SIZE : FE_PTR_SIZE;
        t->align = FE_PTR_ALIGN;
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_SLICE || t->kind == FE_TYPE_STR) {
        t->size = 2UL * FE_PTR_SIZE;
        t->align = FE_PTR_ALIGN;
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_ARRAY) {
        layout_type(ctx, t->elem);
        t->align = t->packed ? 1U : fe_type_align(t->elem);
        t->size = t->length * fe_type_size(t->elem);
        t->cycle_state = 2; return;
    }
    if (t->kind == FE_TYPE_STRUCT) {
        off = 0; max_align = 1;
        for (i = 0; i < t->field_count; ++i) {
            if (!t->fields[i].type && t->fields[i].ast_node)
                t->fields[i].type = fe_type_from_ast(ctx, t->fields[i].ast_node->a);
            layout_type(ctx, t->fields[i].type);
            align = t->packed ? 1U : fe_type_align(t->fields[i].type);
            if (align > max_align) max_align = align;
            off = round_up(off, align);
            t->fields[i].offset = off;
            off += fe_type_size(t->fields[i].type);
        }
        t->align = max_align;
        t->size = round_up(off, max_align);
        t->cycle_state = 2;
        return;
    }
    if (t->kind == FE_TYPE_ENUM) {
        max_size = 0; max_align = 1;
        for (i = 0; i < t->variant_count; ++i) {
            unsigned j;
            off = 0;
            for (j = 0; j < t->variants[i].field_count; ++j) {
                if (!t->variants[i].fields[j].type && t->variants[i].fields[j].ast_node)
                    t->variants[i].fields[j].type = fe_type_from_ast(
                        ctx, t->variants[i].fields[j].ast_node->a);
                layout_type(ctx, t->variants[i].fields[j].type);
                if (fe_type_align(t->variants[i].fields[j].type) > max_align)
                    max_align = fe_type_align(t->variants[i].fields[j].type);
                off += fe_type_size(t->variants[i].fields[j].type);
            }
            if (off > max_size) max_size = off;
        }
        t->bits = t->variant_count > 256U ? 16U : 8U;
        off = round_up(t->bits / 8U, max_align);
        t->size = round_up(off + max_size, max_align);
        t->align = max_align;
        t->cycle_state = 2;
    }
}

void fe_type_layout_all(FeTypeCtx *ctx)
{
    FeType *t;
    for (t = ctx->types; t; t = t->next) layout_type(ctx, t);
}

FeFieldType *fe_type_field(FeType *t, const char *name)
{
    unsigned i;
    if (!t || t->kind != FE_TYPE_STRUCT || !name) return 0;
    for (i = 0; i < t->field_count; ++i)
        if (strcmp(t->fields[i].name, name) == 0) return &t->fields[i];
    return 0;
}

FeVariantType *fe_type_variant(FeType *t, const char *name)
{
    unsigned i;
    if (!t || t->kind != FE_TYPE_ENUM || !name) return 0;
    for (i = 0; i < t->variant_count; ++i)
        if (strcmp(t->variants[i].name, name) == 0) return &t->variants[i];
    return 0;
}

FeType *fe_type_from_ast(FeTypeCtx *ctx, const FeNode *node)
{
    unsigned long length = 0;
    char qualified[128];
    if (!node) return fe_type_intern(ctx, "<unknown>");
    if (node->kind != FE_N_TYPE) return fe_type_intern(ctx, "<unknown>");
    if (node->text && strcmp(node->text, "as") == 0)
        return fe_type_from_ast(ctx, node->b);
    if (node->text && strcmp(node->text, "str") == 0)
        return fe_type_slice(ctx, fe_type_intern(ctx, "u8"));
    if (node->a && node->a->kind==FE_N_IDENT && node->text &&
        strcmp(node->text,"io")==0 && node->a->text) {
        sprintf(qualified,"%s.%s",node->text,node->a->text);
        return fe_type_intern(ctx,qualified);
    }
    if (node->text && (strcmp(node->text, "&") == 0 ||
                       strcmp(node->text, "&mut") == 0))
        return fe_type_ref(ctx, fe_type_from_ast(ctx,node->a),
                           strcmp(node->text,"&mut") == 0);
    if (node->text && strcmp(node->text,"^")==0)
        return fe_type_owned(ctx,fe_type_from_ast(ctx,node->a));
    if (node->text && strcmp(node->text,"?")==0)
        return fe_m7_optional_type(ctx,fe_type_from_ast(ctx,node->a));
    if (node->text && (strcmp(node->text, "[") == 0 ||
                       strcmp(node->text, "[]mut") == 0)) {
        if (node->a) {
            if (node->a->kind == FE_N_LITERAL && node->a->text)
                length = strtoul(node->a->text, 0, 0);
            return fe_type_array(ctx, length, fe_type_from_ast(ctx, node->b));
        }
        return strcmp(node->text,"[]mut")==0 ?
            fe_type_mut_slice(ctx, fe_type_from_ast(ctx,node->b)) :
            fe_type_slice(ctx, fe_type_from_ast(ctx, node->b));
    }
    if (node->text && strcmp(node->text, "!") == 0) {
        if (node->b)
            return fe_m7_error_union_type(ctx,fe_type_from_ast(ctx,node->a),
                                           fe_type_from_ast(ctx,node->b));
        return fe_type_error_union(ctx,fe_type_from_ast(ctx,node->a));
    }
    if (node->text && strcmp(node->text, "*") == 0)
        return fe_type_intern(ctx, "<unknown>");
    if (node->text && strcmp(node->text, "fn") == 0)
        return fe_type_intern(ctx, "<unknown>");
    /* A plain named type may be a generic declaration -- with arguments it is
       an instance, without them it is a mistake -- and only the checker knows
       the declarations, so it decides. */
    if (ctx->instantiate)
        return ctx->instantiate(ctx->instantiate_owner, node);
    return fe_type_intern(ctx, node->text);
}

int fe_type_equal(const FeType *a, const FeType *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (strcmp(a->name, b->name) != 0) return 0;
    /* The same spelling is not the same type across a unit boundary. */
    if (!a->unit || !b->unit) return a->unit == b->unit;
    return strcmp(a->unit, b->unit) == 0;
}

int fe_type_is_integer(const FeType *t)
{
    return t && t->kind == FE_TYPE_INT;
}

int fe_type_is_indexable(const FeType *t)
{
    return t && (t->kind == FE_TYPE_ARRAY || t->kind == FE_TYPE_SLICE ||
                 t->kind == FE_TYPE_STR);
}

const char *fe_type_c_name(const FeType *t, unsigned pointer_bits)
{
    if (!t) return "long";
    if (t->kind == FE_TYPE_OPTIONAL && fe_m7_optional_uses_niche(t->elem))
        return fe_type_c_name(t->elem,pointer_bits);
    if (t->cname) return t->cname;
    if (t->kind == FE_TYPE_VOID) return "void";
    if (t->kind == FE_TYPE_ERROR_UNION) {
        if (t->error_value && t->error_value->kind != FE_TYPE_VOID && t->cname)
            return t->cname;
        return "unsigned short";
    }
    if (t->kind == FE_TYPE_BOOL || t->kind == FE_TYPE_CHAR) return "unsigned char";
    if (t->kind == FE_TYPE_REF) {
        static char ref_name[128];
        if (t->ref_mut) {
            strcpy(ref_name,fe_type_c_name(t->elem,pointer_bits));
            strcat(ref_name," *");
        } else {
            strcpy(ref_name,"const ");
            strcat(ref_name,fe_type_c_name(t->elem,pointer_bits));
            strcat(ref_name," *");
        }
        return ref_name;
    }
    if (t->kind == FE_TYPE_OWNED) {
        static char owned_name[128];
        strcpy(owned_name,fe_type_c_name(t->elem,pointer_bits));
        strcat(owned_name," *");
        return owned_name;
    }
    if (t->kind != FE_TYPE_INT) return "long";
    if (strcmp(t->name, "usize") == 0) return "unsigned long";
    if (strcmp(t->name, "isize") == 0) return "long";
    if (strcmp(t->name, "i8") == 0) return "signed char";
    if (strcmp(t->name, "u8") == 0) return "unsigned char";
    if (strcmp(t->name, "i16") == 0) return "short";
    if (strcmp(t->name, "u16") == 0) return "unsigned short";
    if (strcmp(t->name, "i32") == 0) return "long";
    if (strcmp(t->name, "u32") == 0) return "unsigned long";
    return "long";
}
