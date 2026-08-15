#ifndef FE_PARSER_H
#define FE_PARSER_H

#include "ast.h"

typedef struct FeParser {
    FeLexer lexer;
    FeToken current;
    FeToken previous;
    FeAst *ast;
    FeDiags *diags;
    int forbid_struct_literal;
} FeParser;

void fe_parser_init(FeParser *p, FeAst *ast, const char *src, unsigned long length, const char *file, FeDiags *d);
FeNode *fe_parse_unit(FeParser *p);

#endif
