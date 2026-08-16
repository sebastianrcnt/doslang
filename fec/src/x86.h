#ifndef FE_X86_H
#define FE_X86_H

#include "ir.h"

/* IR to i386 assembly, in the syntax Open Watcom's `wasm` accepts.

   There is no register allocator. Every temporary gets a stack slot, and every
   instruction loads its operands into fixed registers, computes, and stores
   the result back. That is slow code and obviously correct code, and correct
   comes first: a register allocator can be dropped in later without the rest
   of the compiler noticing, because it only changes where a temporary lives. */

void fe_x86_emit(const FeIrModule *m, FILE *out);

#endif
