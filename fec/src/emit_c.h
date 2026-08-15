#ifndef FE_EMIT_C_H
#define FE_EMIT_C_H

#include "check.h"

typedef struct FeEmitter {
    FILE *out;
    FeCheck *check;
    unsigned pointer_bits;
    int indent;
} FeEmitter;

void fe_emit_c_init(FeEmitter *e, FILE *out, FeCheck *check, unsigned pointer_bits);
void fe_emit_c_program(FeEmitter *e);

#endif
