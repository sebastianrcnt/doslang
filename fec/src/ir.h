#ifndef FE_IR_H
#define FE_IR_H

#include "arena.h"
#include <stdio.h>

/* The intermediate representation. `IR.md` is the description; this is the
   shape it takes in memory.

   Ferro types do not survive into here. A struct, a slice, an optional and an
   error union are all `mem<N>`, and a field is a byte offset that lowering
   worked out. The machine types are what a register can hold plus a size. */

typedef enum FeIrType {
    FE_IR_VOID,
    FE_IR_I8, FE_IR_I16, FE_IR_I32,
    FE_IR_PTR,
    FE_IR_MEM          /* size lives on the value or slot */
} FeIrType;

typedef enum FeIrOp {
    FE_IR_CONST, FE_IR_LOAD, FE_IR_STORE, FE_IR_ADDR,
    FE_IR_ADD, FE_IR_SUB, FE_IR_MUL, FE_IR_DIV, FE_IR_MOD,
    FE_IR_AND, FE_IR_OR, FE_IR_XOR, FE_IR_SHL, FE_IR_SHR,
    FE_IR_EQ, FE_IR_NE, FE_IR_LT, FE_IR_LE, FE_IR_GT, FE_IR_GE,
    FE_IR_CAST, FE_IR_CALL, FE_IR_COPY
} FeIrOp;

typedef enum FeIrTerm {
    FE_IR_JMP, FE_IR_BR, FE_IR_RET, FE_IR_TRAP
} FeIrTerm;

/* Why a program stopped. Kept small and stable: it is a number in the
   executable, and the runtime turns it back into words. */
typedef enum FeIrTrap {
    FE_TRAP_BOUNDS = 0,
    FE_TRAP_OVERFLOW = 1,
    FE_TRAP_DIVIDE = 2,
    FE_TRAP_UNREACHABLE = 3,
    FE_TRAP_EXPLICIT = 4
} FeIrTrap;

/* Where an instruction reads or writes. A place is a base plus a constant
   offset; anything computed goes through a pointer temporary instead. */
typedef enum FeIrBase {
    FE_PLACE_LOCAL,    /* $n     */
    FE_PLACE_GLOBAL,   /* @name  */
    FE_PLACE_TEMP      /* %p     */
} FeIrBase;

typedef struct FeIrPlace {
    FeIrBase base;
    unsigned index;          /* local or temp number */
    const char *name;        /* global name */
    long offset;
} FeIrPlace;

typedef struct FeIrValue {
    FeIrOp op;
    FeIrType type;
    unsigned dest;           /* %dest, or 0 when the op has no result */
    int has_dest;
    /* operands, by role -- only the ones the op uses are set */
    unsigned a, b;           /* temporaries */
    long imm;                /* const, cast width, copy size */
    FeIrPlace place;         /* load / store / addr / copy destination */
    FeIrPlace place2;        /* copy source */
    const char *callee;
    unsigned *args;
    unsigned arg_count;
    int is_unsigned;         /* picks the signed or unsigned instruction */
    unsigned long line;      /* for diagnostics that survive into the backend */
    struct FeIrValue *next;
} FeIrValue;

typedef struct FeIrFunc FeIrFunc;

typedef struct FeIrBlock {
    unsigned id;
    FeIrFunc *func;          /* the function this block is being built in */
    FeIrValue *first;
    FeIrValue *last;
    FeIrTerm term;
    unsigned cond;           /* br */
    unsigned target;         /* jmp, br true */
    unsigned target_else;    /* br false */
    unsigned ret_value;      /* ret */
    int has_ret_value;
    FeIrTrap trap;
    unsigned long trap_line;
    /* Set once a terminator is chosen. Lowering asks before appending a
       jump, so a `return` inside a branch is not overwritten by the jump
       to the join block. */
    int terminated;
    struct FeIrBlock *next;
} FeIrBlock;

typedef struct FeIrLocal {
    FeIrType type;
    unsigned long size;      /* for FE_IR_MEM */
    unsigned align;
    const char *name;        /* the Ferro name, for reading the dump */
} FeIrLocal;

struct FeIrFunc {
    const char *name;        /* unit.name */
    FeIrType ret;
    unsigned long ret_size;  /* when ret is FE_IR_MEM */
    /* A function returning mem<N> takes the address to write as a hidden
       first parameter, so the caller owns the storage. */
    int returns_by_address;
    FeIrLocal *locals;
    unsigned local_count;
    unsigned local_capacity;
    unsigned param_count;    /* the first `param_count` locals are parameters */
    unsigned temp_count;
    FeIrBlock *first;
    FeIrBlock *last;
    unsigned block_count;
    int is_extern;
    struct FeIrFunc *next;
};

typedef struct FeIrGlobal {
    const char *name;
    FeIrType type;
    unsigned long size;
    unsigned align;
    const unsigned char *init;   /* size bytes, or null for zero */
    struct FeIrGlobal *next;
} FeIrGlobal;

typedef struct FeIrModule {
    FeArena arena;
    const char *unit_file;   /* the one file-name string a unit's traps share */
    /* The entry unit's `main`, if it has one. The runtime's start stub
       calls a fixed name, so the generator emits a jump to this one. */
    const char *entry_main;
    FeIrFunc *funcs;
    FeIrFunc *last_func;
    FeIrGlobal *globals;
    FeIrGlobal *last_global;
} FeIrModule;

void fe_ir_module_init(FeIrModule *m);
void fe_ir_module_destroy(FeIrModule *m);

FeIrFunc *fe_ir_func(FeIrModule *m, const char *name, FeIrType ret,
                     unsigned long ret_size);
unsigned fe_ir_local(FeIrModule *m, FeIrFunc *f, FeIrType type,
                     unsigned long size, unsigned align, const char *name);
unsigned fe_ir_temp(FeIrFunc *f);
FeIrBlock *fe_ir_block(FeIrModule *m, FeIrFunc *f);

/* Places */
FeIrPlace fe_ir_at_local(unsigned index, long offset);
FeIrPlace fe_ir_at_global(const char *name, long offset);
FeIrPlace fe_ir_at_temp(unsigned temp, long offset);

/* Instructions. Each returns the destination temporary where there is one. */
unsigned fe_ir_const(FeIrModule *m, FeIrBlock *b, FeIrType t, long v);
unsigned fe_ir_load(FeIrModule *m, FeIrBlock *b, FeIrType t, FeIrPlace p);
/* `t` is how wide the write is. Without it a one-byte value would be stored
   four bytes wide and take its neighbours with it. */
void     fe_ir_store(FeIrModule *m, FeIrBlock *b, FeIrPlace p, unsigned v,
                     FeIrType t);
unsigned fe_ir_addr(FeIrModule *m, FeIrBlock *b, FeIrPlace p);
unsigned fe_ir_binary(FeIrModule *m, FeIrBlock *b, FeIrOp op, FeIrType t,
                      unsigned a, unsigned c, int is_unsigned);
unsigned fe_ir_cast(FeIrModule *m, FeIrBlock *b, FeIrType from, FeIrType to,
                    unsigned a, int is_unsigned);
unsigned fe_ir_call(FeIrModule *m, FeIrBlock *b, FeIrType ret,
                    const char *callee, unsigned *args, unsigned count);
void     fe_ir_copy(FeIrModule *m, FeIrBlock *b, FeIrPlace dst, FeIrPlace src,
                    unsigned long size);

/* Terminators */
void fe_ir_jmp(FeIrBlock *b, unsigned target);
void fe_ir_br(FeIrBlock *b, unsigned cond, unsigned t, unsigned f);
void fe_ir_ret(FeIrBlock *b, unsigned value, int has_value);
void fe_ir_trap(FeIrBlock *b, FeIrTrap reason, unsigned long line);

void fe_ir_dump(const FeIrModule *m, FILE *out);
const char *fe_ir_type_name(FeIrType t);
const char *fe_ir_op_name(FeIrOp op);

#endif
