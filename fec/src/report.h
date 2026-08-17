#ifndef FE_REPORT_H
#define FE_REPORT_H

#include "check.h"
#include <stdio.h>

/* How much of the build is outside what the checker promises. */
void fe_report_unsafe(const FeBuild *build, FILE *out);

/* What the generic instances came to. */
void fe_report_instances(const FeCheck *c, FILE *out);

#endif
