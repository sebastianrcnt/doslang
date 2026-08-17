#ifndef FE_WORD_H
#define FE_WORD_H

/* The target's word. i386 protected mode flat, so 32 bits (SPEC 2).

   Anything that means something *to the target* -- a constant, a byte offset,
   an object's size, a line number that ends up in the executable -- is carried
   in these and never in `long`.

   `long` is 4 bytes on i386 and on Windows, 8 bytes on 64-bit Unix. A constant
   like the FNV-1a basis 0x811C9DC5 therefore prints as -2128831035 when the
   compiler was built 32-bit and 2166136261 when it was built 64-bit. Same bits,
   same instruction after the assembler -- but different text. A compiler whose
   output depends on the machine it ran on cannot be checked by comparing its
   output, which is how `tests/determ.py` checks it.

   `long` is still right for what never leaves the compiler: loop counters,
   buffer indices, host-side sizes.

   The rule this encodes: the IR speaks the target's widths only. The host's
   widths do not cross into it. Widening for a 64-bit target one day is then a
   deliberate change here rather than an accident of where the build ran. */

typedef int FeI32;
typedef unsigned FeU32;

/* `int` is 32 bits on every host we build for (i386, x86-64, ARM64). Where it
   is not, this fails to compile instead of quietly emitting different code. */
typedef char fe_word_is_32bit[sizeof(FeI32) == 4 && sizeof(FeU32) == 4 ? 1 : -1];

#endif
