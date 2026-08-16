#ifndef FE_CHECK_H
#define FE_CHECK_H

#include "types.h"
#include "diag.h"
#include "resolve.h"

typedef struct FeScope FeScope;

/* The checker spans a whole build, not one file. Names cross unit boundaries,
   so every unit's declarations have to exist before any unit's bodies are
   looked at, and they all have to be interned in one type context or the same
   spelling in two units would not be the same type. */
typedef struct FeCheck {
    /* Scopes, symbols and types outlive whichever unit is current, so they
       come from the checker's own arena rather than from an AST's. */
    FeArena arena;
    FeBuild *build;
    FeAst *ast;                          /* the unit being checked now */
    FeUnit *unit;                        /* its entry in the build */
    FeScope *unit_scope[FE_BUILD_UNIT_MAX];
    FeTypeCtx types;
    FeDiags *diags;
    unsigned pointer_bits;
    unsigned local_serial;
    int no_checks;
} FeCheck;

void fe_check_init(FeCheck *c, FeBuild *build, FeDiags *diags,
                   unsigned pointer_bits, int no_checks);
void fe_check_destroy(FeCheck *c);
int fe_check_program(FeCheck *c);

#endif
