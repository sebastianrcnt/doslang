#ifndef FE_AST_H
#define FE_AST_H

#include "arena.h"
#include "lexer.h"
#include <stdio.h>

typedef enum FeNodeKind {
    FE_N_UNIT, FE_N_IMPORT, FE_N_FN, FE_N_STRUCT, FE_N_ENUM, FE_N_ERROR_DECL, FE_N_CONST,
    FE_N_GLOBAL, FE_N_FIELD, FE_N_PARAM, FE_N_VARIANT, FE_N_BLOCK, FE_N_LET, FE_N_VAR,
    FE_N_EXPR_STMT, FE_N_ASSIGN, FE_N_IF, FE_N_WHILE, FE_N_FOR, FE_N_MATCH, FE_N_ARM,
    FE_N_RETURN, FE_N_BREAK, FE_N_CONTINUE, FE_N_DEFER, FE_N_UNSAFE, FE_N_ASM,
    FE_N_TYPE, FE_N_EXPR, FE_N_BINARY, FE_N_UNARY, FE_N_CALL, FE_N_INDEX, FE_N_MEMBER,
    FE_N_LITERAL, FE_N_IDENT, FE_N_STRUCT_INIT, FE_N_ARRAY_INIT, FE_N_ERROR_NODE
} FeNodeKind;

typedef struct FeNode FeNode;
typedef struct FeType FeType;
struct FeNode {
    FeNodeKind kind;
    FeLoc loc;
    char *text;
    FeNode *a;
    FeNode *b;
    FeNode *c;
    FeNode *children;
    FeNode *next;
    /* Semantic information filled by checking; kept out of AST dumps. */
    char *cname;
    char *aux_text;
    char *aux_cname;
    FeType *sem_type;
    /* Expected contextual wrapper, used by M7 for null/Some and E!T
       success/failure construction without mutating the expression's type. */
    FeType *sem_context;
    FeNode *sem_decl;
    unsigned flags;
};

/* Bits in FeNode.flags. 0x100 and above belong to own.h. */
#define FE_NODE_PACKED 0x1U
#define FE_NODE_STATIC 0x2U
#define FE_NODE_SHARED 0x4U
#define FE_NODE_PUB    0x8U

typedef struct FeAst {
    FeArena arena;
    FeNode *root;
} FeAst;

void fe_ast_init(FeAst *a);
void fe_ast_destroy(FeAst *a);
FeNode *fe_node(FeAst *a, FeNodeKind k, FeLoc loc, const char *text, unsigned long len);
void fe_node_add(FeNode *parent, FeNode *child);
void fe_ast_dump(const FeNode *n, int indent, FILE *out);
const char *fe_node_name(FeNodeKind k);

#endif
