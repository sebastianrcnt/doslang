#include "types.h"
#include <string.h>

void fe_types_init(FeTypeCtx *ctx, FeArena *arena, unsigned pointer_bits)
{ ctx->arena=arena; ctx->types=0; ctx->pointer_bits=pointer_bits; }

FeType *fe_type_intern(FeTypeCtx *ctx, const char *name)
{
    FeType *t; unsigned i; unsigned bits=0; int uns=0; FeTypeKind kind=FE_TYPE_UNKNOWN;
    if (!name) name="<unknown>";
    for (t=ctx->types;t;t=t->next) if(strcmp(t->name,name)==0) return t;
    if(strcmp(name,"void")==0) kind=FE_TYPE_VOID;
    else if(strcmp(name,"bool")==0) kind=FE_TYPE_BOOL;
    else if(strcmp(name,"i8")==0||strcmp(name,"u8")==0){kind=FE_TYPE_INT;bits=8;uns=name[0]=='u';}
    else if(strcmp(name,"i16")==0||strcmp(name,"u16")==0){kind=FE_TYPE_INT;bits=16;uns=name[0]=='u';}
    else if(strcmp(name,"i32")==0||strcmp(name,"u32")==0){kind=FE_TYPE_INT;bits=32;uns=name[0]=='u';}
    else if(strcmp(name,"usize")==0||strcmp(name,"isize")==0){kind=FE_TYPE_INT;bits=ctx->pointer_bits;uns=name[0]=='u';}
    t=(FeType *)fe_arena_alloc(ctx->arena,sizeof(FeType)); if(!t)return 0;
    for(i=0;i<sizeof(t->name)-1 && name[i];i++) t->name[i]=name[i];
    t->name[i]='\0';
    t->kind=kind;t->bits=bits;t->is_unsigned=uns;t->next=ctx->types;ctx->types=t;return t;
}

FeType *fe_type_from_ast(FeTypeCtx *ctx, const FeNode *node)
{
    if(!node)return fe_type_intern(ctx,"<unknown>");
    if(node->kind!=FE_N_TYPE)return fe_type_intern(ctx,"<unknown>");
    if(node->text && (strcmp(node->text,"?")==0||strcmp(node->text,"!")==0||strcmp(node->text,"^")==0||strcmp(node->text,"&")==0||strcmp(node->text,"*")==0||strcmp(node->text,"far")==0))return fe_type_intern(ctx,"<unknown>");
    if(node->text && strcmp(node->text,"as")==0)return fe_type_from_ast(ctx,node->b);
    if(node->text && strcmp(node->text,"fn")==0)return fe_type_intern(ctx,"<unknown>");
    return fe_type_intern(ctx,node->text);
}
int fe_type_equal(const FeType *a,const FeType *b){return a==b || (a&&b&&strcmp(a->name,b->name)==0);}
int fe_type_is_integer(const FeType *t){return t&&t->kind==FE_TYPE_INT;}
const char *fe_type_c_name(const FeType *t,unsigned pointer_bits)
{
    if(!t)return "int";
    if(t->kind==FE_TYPE_VOID)return "void";
    if(t->kind==FE_TYPE_BOOL)return "unsigned char";
    if(t->kind!=FE_TYPE_INT)return "int";
    if(strcmp(t->name,"usize")==0)return pointer_bits==16?"unsigned short":"unsigned long";
    if(strcmp(t->name,"isize")==0)return pointer_bits==16?"short":"long";
    if(strcmp(t->name,"i8")==0)return "signed char";
    if(strcmp(t->name,"u8")==0)return "unsigned char";
    if(strcmp(t->name,"i16")==0)return "short";
    if(strcmp(t->name,"u16")==0)return "unsigned short";
    if(strcmp(t->name,"i32")==0)return "long";
    if(strcmp(t->name,"u32")==0)return "unsigned long";
    return "int";
}
