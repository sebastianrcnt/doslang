#ifndef FE_LOWERPRI_H
#define FE_LOWERPRI_H

/* Lowering's own vocabulary, shared by the files it is split across. */

#include "lower.h"
#include "m7.h"
#include "own.h"
#include <string.h>
#include <stdio.h>

#include <string.h>
#include "m7.h"
#include "own.h"
#include <stdio.h>

/* ------------------------------------------------------------------------- *
 * Lowering
 *
 * One function at a time, one statement at a time. A `Slot` is what an
 * expression produced: either a value already in a temporary, or a place in
 * memory that a value can be read from or written to. Aggregates are always
 * places -- they are never carried in a temporary, because a temporary is a
 * register and an aggregate does not fit in one.
 * ------------------------------------------------------------------------- */


typedef struct LowerVar {
    const char *cname;
    unsigned local;
    /* An aggregate parameter arrives as an address, so the slot holds a
       pointer and the value is one dereference away. */
    int by_address;
} LowerVar;

typedef struct Lower {
    FeCheck *c;
    FeIrModule *m;
    FeIrFunc *fn;
    FeIrBlock *b;              /* the block being appended to */
    FeType *ret_type;
    unsigned ret_local;        /* hidden result address, when returning mem */
    /* These three grow. A fixed size here does not report a program that is
       too big -- it quietly drops what does not fit and generates wrong code,
       which is the worst way for a limit to be reached. */
    LowerVar *vars;
    unsigned var_count;
    unsigned var_capacity;
    /* Loop targets, for break and continue. */
    unsigned break_target[32];
    unsigned continue_target[32];
    unsigned loop_depth;
    /* What a scope still owes when it ends: `defer` blocks to run and owned
       values to release, in the order they were written. Every exit path runs
       what is live, last first.

       A drop carries a flag beside the value. The flag is set when the value
       is stored and cleared wherever it is moved away, so the release happens
       exactly on the paths where the value is still there -- which is not
       something the shape of the code can tell you on its own. */
    struct {
        FeNode *block;         /* a `defer`, when set */
        unsigned local;        /* the owned value, otherwise */
        unsigned flag;
        FeType *type;
    } *owed;
    unsigned owed_count;
    unsigned owed_capacity;
    /* Every `error.Name` used anywhere in the build, sorted, numbered from one.
       SPEC 4.6: the names are collected rather than declared, and the order is
       fixed by the spelling so that the same program always gets the same
       codes however the build was ordered. */
    const char **error_names;
    unsigned error_count;
    unsigned error_capacity;
    int failed;
} Lower;

typedef struct Slot {
    int is_place;
    unsigned temp;             /* the value, when is_place is 0 */
    FeIrPlace place;           /* where it lives, when is_place is 1 */
    FeIrType type;
    unsigned long size;        /* for FE_IR_MEM */
} Slot;



/* A slice is a pointer and a length, in that order. Both the compiler and the
   runtime read it this way, so the offsets live here and nowhere else. */
#define SLICE_PTR_OFFSET 0L
#define SLICE_LEN_OFFSET 4L

/* Grow one of the checker's own arrays. Returns zero when there is no more
   memory, which the caller reports rather than ignores. */
int lower_reserve(Lower *L, void **items, unsigned *capacity, unsigned needed,
                  unsigned long item_size);

/* Every definition in lowering, so the split files can see each other. */
FeIrType tag_type_of(const FeType *t);
int struct_is_generic(const FeNode *decl);
void lower_if_let(Lower *L, FeNode *n);
unsigned wrapper_tag(Lower *L, Slot w, const FeType *t, FeNode *n);
void bind_payload(Lower *L, Slot subject, const FeType *t,
                  const FeVariantType *v, FeNode *arm);
void fail(Lower *L, const char *why, FeNode *n);
FeIrType ir_type_of(const FeType *t);
int enum_has_payload(const FeType *t);
FeIrType ir_type(const FeType *t);
unsigned long ir_size(const FeType *t);
unsigned ir_align(const FeType *t);
int type_is_unsigned(const FeType *t);
Slot slot_value(unsigned temp, FeIrType t);
Slot slot_place(FeIrPlace p, FeIrType t, unsigned long size);
Slot slot_void(void);
unsigned as_value(Lower *L, Slot s, FeNode *n);
unsigned as_address(Lower *L, Slot s, FeNode *n);
int needs_release(const FeType *t);
unsigned declare_var(Lower *L, const char *cname, const FeType *t,
                            const char *name);
int release_flag(Lower *L, unsigned local, unsigned *flag);
LowerVar *find_var(Lower *L, const char *cname);
FeIrBlock *new_block(Lower *L);
void guard(Lower *L, unsigned ok, FeIrTrap reason, unsigned long line);
FeIrType tag_type(const FeType *t);
int uses_niche(const FeType *t);
unsigned scratch(Lower *L, const FeType *t, const char *why);
void indexable_parts(Lower *L, Slot base, const FeType *t,
                            unsigned *data, unsigned *length, FeNode *n);
void note_error_name(Lower *L, const char *name);
void collect_error_names(Lower *L, FeNode *n);
long error_code(Lower *L, const char *name);
FeIrOp binary_op(const char *op, int *is_cmp);
long literal_value(FeNode *n);
Slot lower_logical(Lower *L, FeNode *n, int is_and);
int lower_builtin(Lower *L, FeNode *n, Slot *out);
int is_mem_call(const FeNode *n, const char *what);
Slot allocation_result(Lower *L, FeNode *n, unsigned pointer);
int lower_mem(Lower *L, FeNode *n, Slot *out);
void emit_text(Lower *L, unsigned handle, const char *text,
                      unsigned long len);
void emit_value_text(Lower *L, unsigned handle, FeNode *arg, int verb);
int lower_print(Lower *L, FeNode *n, Slot *out);
Slot lower_call(Lower *L, FeNode *n);
Slot lower_expr(Lower *L, FeNode *n);
Slot lower_expr_core(Lower *L, FeNode *n);
const char *drop_name(Lower *L, const FeType *t);
void run_deferred(Lower *L, unsigned from);
Slot wrap_context(Lower *L, Slot v, FeNode *n);
unsigned wrapper_tag(Lower *L, Slot w, const FeType *t, FeNode *n);
Slot wrapper_payload(Lower *L, Slot w, const FeType *t);
void return_error(Lower *L, unsigned err, FeNode *n);
Slot lower_try(Lower *L, FeNode *n);
Slot lower_lazy(Lower *L, FeNode *n, int is_catch);
void store_into(Lower *L, FeIrPlace dst, Slot value, FeNode *n,
                       unsigned long size);
void lower_return(Lower *L, FeNode *n);
void lower_if(Lower *L, FeNode *n);
void lower_while(Lower *L, FeNode *n);
Slot lower_slice(Lower *L, FeNode *n);
void lower_for(Lower *L, FeNode *n);
void lower_match(Lower *L, FeNode *n);
void lower_stmt(Lower *L, FeNode *n);
void lower_global(Lower *L, FeNode *n);
int fn_is_generic(const FeNode *fn);
void lower_fn_as(Lower *L, FeNode *fn, const char *name);
void lower_fn(Lower *L, FeNode *fn);

#endif
