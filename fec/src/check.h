#ifndef FE_CHECK_H
#define FE_CHECK_H

#include "types.h"
#include "diag.h"

typedef struct FeCheck {
    FeAst *ast;
    FeTypeCtx types;
    FeDiags *diags;
    unsigned pointer_bits;
    unsigned local_serial;
    int no_checks;
} FeCheck;

void fe_check_init(FeCheck *c, FeAst *ast, FeDiags *diags,
                   unsigned pointer_bits, int no_checks);
int fe_check_program(FeCheck *c);
FeType *fe_check_expr_type(FeCheck *c, FeNode *n);

#endif
