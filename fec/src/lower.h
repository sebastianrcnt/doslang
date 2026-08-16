#ifndef FE_LOWER_H
#define FE_LOWER_H

#include "check.h"
#include "ir.h"

/* Turn the checked program into IR.

   The checker leaves every expression with a type and every declaration with a
   link-visible name; lowering reads those and produces the flat form the
   backend wants. Everything Ferro-shaped is expanded here -- `try` becomes a
   branch, `defer` is copied onto each exit path, an index becomes a comparison
   and a trap -- so that neither the checker nor the backend has to know about
   the other's world. */

int fe_lower_program(FeCheck *c, FeIrModule *out);

#endif
