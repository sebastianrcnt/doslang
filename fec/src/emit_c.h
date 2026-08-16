#ifndef FE_EMIT_C_H
#define FE_EMIT_C_H

#include "check.h"

typedef struct FeEmitter {
    FILE *out;
    FeCheck *check;
    unsigned pointer_bits;
    int indent;
    int no_checks;
    unsigned temp_serial;
    FeNode *fallthrough_block;
    FeNode *block_stack[32];
    unsigned block_seen[32];
    unsigned block_depth;
    unsigned loop_floor[16];
    unsigned loop_depth;
    FeType *current_ret;
    FeNode *current_fn;
} FeEmitter;

void fe_emit_c_init(FeEmitter *e, FILE *out, FeCheck *check,
                    unsigned pointer_bits, int no_checks);
void fe_emit_c_program(FeEmitter *e);

#endif
