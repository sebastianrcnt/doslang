#ifndef FE_DIAG_H
#define FE_DIAG_H

#include <stdio.h>

typedef struct FeLoc {
    const char *file;
    unsigned long line;
    unsigned long col;
} FeLoc;

typedef struct FeDiags {
    unsigned long errors;
    unsigned long warnings;
} FeDiags;

void fe_diag_error(FeDiags *d, FeLoc loc, const char *msg);
void fe_diag_errorf(FeDiags *d, FeLoc loc, const char *msg, const char *arg);
void fe_diag_note(FeLoc loc, const char *msg);

#endif
