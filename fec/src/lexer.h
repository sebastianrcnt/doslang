#ifndef FE_LEXER_H
#define FE_LEXER_H

#include "diag.h"
#include "arena.h"

typedef enum FeTokKind {
    FE_TOK_EOF, FE_TOK_ERROR, FE_TOK_IDENT, FE_TOK_INT, FE_TOK_CHAR, FE_TOK_STRING,
    FE_TOK_UNIT, FE_TOK_IMPORT, FE_TOK_PUB, FE_TOK_FN, FE_TOK_STRUCT, FE_TOK_ENUM,
    FE_TOK_ERROR_KW, FE_TOK_CONST, FE_TOK_STATIC, FE_TOK_VAR, FE_TOK_LET, FE_TOK_MUT,
    FE_TOK_IF, FE_TOK_ELSE, FE_TOK_WHILE, FE_TOK_FOR, FE_TOK_IN, FE_TOK_MATCH,
    FE_TOK_RETURN, FE_TOK_BREAK, FE_TOK_CONTINUE, FE_TOK_DEFER, FE_TOK_UNSAFE,
    FE_TOK_COMPTIME, FE_TOK_ASM, FE_TOK_TRY, FE_TOK_CATCH, FE_TOK_AS, FE_TOK_EXTERN,
    FE_TOK_INTERRUPT, FE_TOK_INTERRUPT_SAFE, FE_TOK_FAR, FE_TOK_TRUE, FE_TOK_FALSE, FE_TOK_NULL,
    FE_TOK_UNDEFINED, FE_TOK_SHARED, FE_TOK_ATOMIC, FE_TOK_CRITICAL, FE_TOK_SELF,
    FE_TOK_SELFTYPE, FE_TOK_TYPE, FE_TOK_PACKED, FE_TOK_ORELSE,
    FE_TOK_LPAREN, FE_TOK_RPAREN, FE_TOK_LBRACE, FE_TOK_RBRACE, FE_TOK_LBRACKET, FE_TOK_RBRACKET,
    FE_TOK_COMMA, FE_TOK_SEMI, FE_TOK_COLON, FE_TOK_DOT, FE_TOK_DOTDOT,
    FE_TOK_PLUS, FE_TOK_MINUS, FE_TOK_STAR, FE_TOK_SLASH, FE_TOK_PERCENT,
    FE_TOK_PLUS_EQ, FE_TOK_MINUS_EQ, FE_TOK_STAR_EQ, FE_TOK_SLASH_EQ, FE_TOK_PERCENT_EQ,
    FE_TOK_PLUS_WRAP, FE_TOK_MINUS_WRAP, FE_TOK_STAR_WRAP,
    FE_TOK_EQ, FE_TOK_EQEQ, FE_TOK_NE, FE_TOK_LT, FE_TOK_LE, FE_TOK_GT, FE_TOK_GE,
    FE_TOK_AND, FE_TOK_OR, FE_TOK_AND_KW, FE_TOK_OR_KW, FE_TOK_XOR, FE_TOK_NOT, FE_TOK_BANG, FE_TOK_SHL, FE_TOK_SHR,
    FE_TOK_AND_EQ, FE_TOK_OR_EQ, FE_TOK_XOR_EQ, FE_TOK_SHL_EQ, FE_TOK_SHR_EQ,
    FE_TOK_ANDAND, FE_TOK_OROR, FE_TOK_ARROW, FE_TOK_FATARROW, FE_TOK_AT,
    FE_TOK_QUESTION, FE_TOK_UNKNOWN
} FeTokKind;

typedef struct FeToken {
    FeTokKind kind;
    const char *begin;
    unsigned long length;
    FeLoc loc;
} FeToken;

typedef struct FeLexer {
    const char *src;
    unsigned long length;
    unsigned long pos;
    unsigned long line;
    unsigned long col;
    const char *file;
    FeDiags *diags;
} FeLexer;

void fe_lexer_init(FeLexer *l, const char *src, unsigned long length, const char *file, FeDiags *d);
FeToken fe_lexer_next(FeLexer *l);
const char *fe_token_name(FeTokKind k);

#endif
