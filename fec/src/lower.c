#include "lower.h"
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

#define LOWER_MAX_LOCALS 256

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
    LowerVar vars[LOWER_MAX_LOCALS];
    unsigned var_count;
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
    } owed[64];
    unsigned owed_count;
    /* Every `error.Name` used anywhere in the build, sorted, numbered from one.
       SPEC 4.6: the names are collected rather than declared, and the order is
       fixed by the spelling so that the same program always gets the same
       codes however the build was ordered. */
    const char *error_names[256];
    unsigned error_count;
    int failed;
} Lower;

typedef struct Slot {
    int is_place;
    unsigned temp;             /* the value, when is_place is 0 */
    FeIrPlace place;           /* where it lives, when is_place is 1 */
    FeIrType type;
    unsigned long size;        /* for FE_IR_MEM */
} Slot;

static Slot lower_expr(Lower *L, FeNode *n);
static void lower_stmt(Lower *L, FeNode *n);
static void store_into(Lower *L, FeIrPlace dst, Slot value, FeNode *n,
                       unsigned long size);
static void lower_for(Lower *L, FeNode *n);
static int lower_mem(Lower *L, FeNode *n, Slot *out);
static long error_code(Lower *L, const char *name);
static int fn_is_generic(const FeNode *fn);
static void lower_fn_as(Lower *L, FeNode *fn, const char *name);
static Slot lower_slice(Lower *L, FeNode *n);
static void guard(Lower *L, unsigned ok, FeIrTrap reason, unsigned long line);
static void lower_match(Lower *L, FeNode *n);
static int enum_has_payload(const FeType *t);
static void lower_global(Lower *L, FeNode *n);
static long literal_value(FeNode *n);
static Slot wrap_context(Lower *L, Slot v, FeNode *n);
static Slot lower_try(Lower *L, FeNode *n);
static Slot lower_lazy(Lower *L, FeNode *n, int is_catch);
static Slot wrapper_payload(Lower *L, Slot w, const FeType *t);
static unsigned scratch(Lower *L, const FeType *t, const char *why);
static int uses_niche(const FeType *t);
static FeIrType tag_type(const FeType *t);
static void run_deferred(Lower *L, unsigned from);
static unsigned declare_var(Lower *L, const char *cname, const FeType *t,
                            const char *name);
static void indexable_parts(Lower *L, Slot base, const FeType *t,
                            unsigned *data, unsigned *length, FeNode *n);

static void fail(Lower *L, const char *why, FeNode *n)
{
    if (L->failed) return;
    L->failed = 1;
    fprintf(fe_diag_stream(), "%s:%lu:%lu: internal: cannot lower %s\n",
            n && n->loc.file ? n->loc.file : "?",
            n ? n->loc.line : 0UL, n ? n->loc.col : 0UL, why);
}

/* ---------------------------------------------------------------- types --- */

/* A Ferro type becomes what a register can hold, or a size in memory. Anything
   with more than one field is memory: the backend never has to decide whether
   an aggregate fits somewhere. */
static FeIrType ir_type_of(const FeType *t)
{
    if (!t) return FE_IR_VOID;
    switch (t->kind) {
    case FE_TYPE_VOID:  return FE_IR_VOID;
    case FE_TYPE_BOOL:
    case FE_TYPE_CHAR:  return FE_IR_I8;
    case FE_TYPE_INT:
        if (t->bits <= 8U) return FE_IR_I8;
        if (t->bits <= 16U) return FE_IR_I16;
        return FE_IR_I32;
    case FE_TYPE_REF:   return FE_IR_PTR;
    case FE_TYPE_OWNED:
        /* An owned slice carries a length beside the pointer. */
        return t->elem && t->elem->kind == FE_TYPE_SLICE ? FE_IR_MEM : FE_IR_PTR;
    case FE_TYPE_ENUM:
        /* A payload-free enum is just its tag. */
        return t->variant_count && t->fields ? FE_IR_MEM :
               (t->size <= 1UL ? FE_IR_I8 :
                t->size <= 2UL ? FE_IR_I16 : FE_IR_I32);
    default:
        return FE_IR_MEM;
    }
}

static int enum_has_payload(const FeType *t)
{
    unsigned i;
    if (!t || t->kind != FE_TYPE_ENUM) return 0;
    for (i = 0; i < t->variant_count; ++i)
        if (t->variants[i].field_count) return 1;
    return 0;
}

static FeIrType ir_type(const FeType *t)
{
    if (t && t->kind == FE_TYPE_ENUM && enum_has_payload(t)) return FE_IR_MEM;
    return ir_type_of(t);
}

static unsigned long ir_size(const FeType *t)
{
    return t ? fe_type_size(t) : 0UL;
}

static unsigned ir_align(const FeType *t)
{
    return t ? fe_type_align(t) : 1U;
}

static int type_is_unsigned(const FeType *t)
{
    return t && t->kind == FE_TYPE_INT && t->is_unsigned;
}

/* ---------------------------------------------------------------- slots --- */

static Slot slot_value(unsigned temp, FeIrType t)
{
    Slot s;
    s.is_place = 0; s.temp = temp; s.type = t; s.size = 0;
    s.place = fe_ir_at_temp(0, 0);
    return s;
}

static Slot slot_place(FeIrPlace p, FeIrType t, unsigned long size)
{
    Slot s;
    s.is_place = 1; s.temp = 0; s.place = p; s.type = t; s.size = size;
    return s;
}

static Slot slot_void(void)
{
    return slot_value(0, FE_IR_VOID);
}

/* Read a slot as a value. An aggregate has no value form, so asking for one is
   a lowering bug rather than a program error. */
static unsigned as_value(Lower *L, Slot s, FeNode *n)
{
    if (!s.is_place) return s.temp;
    if (s.type == FE_IR_MEM) { fail(L, "an aggregate as a value", n); return 0; }
    return fe_ir_load(L->m, L->b, s.type, s.place);
}

/* The address of a slot. */
static unsigned as_address(Lower *L, Slot s, FeNode *n)
{
    if (!s.is_place) { fail(L, "the address of a temporary", n); return 0; }
    return fe_ir_addr(L->m, L->b, s.place);
}

/* --------------------------------------------------------------- locals --- */

/* Does letting go of this type have to do something? */
static int needs_release(const FeType *t)
{
    if (!t) return 0;
    if (t->kind == FE_TYPE_OWNED) return 1;
    return t->has_drop != 0;
}

static unsigned declare_var(Lower *L, const char *cname, const FeType *t,
                            const char *name)
{
    unsigned local = fe_ir_local(L->m, L->fn, ir_type(t), ir_size(t),
                                 ir_align(t), name);
    if (L->var_count < LOWER_MAX_LOCALS) {
        L->vars[L->var_count].cname = cname;
        L->vars[L->var_count].local = local;
        L->vars[L->var_count].by_address = 0;
        ++L->var_count;
    }
    if (needs_release(t) && L->owed_count < 64) {
        unsigned flag = fe_ir_local(L->m, L->fn, FE_IR_I8, 1, 1, "live");
        unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I8, 0);
        fe_ir_store(L->m, L->b, fe_ir_at_local(flag, 0), zero, FE_IR_I8);
        L->owed[L->owed_count].block = 0;
        L->owed[L->owed_count].local = local;
        L->owed[L->owed_count].flag = flag;
        L->owed[L->owed_count].type = (FeType *)t;
        ++L->owed_count;
    }
    return local;
}

/* The liveness flag beside a local, or none. */
static int release_flag(Lower *L, unsigned local, unsigned *flag)
{
    unsigned i;
    for (i = L->owed_count; i > 0; --i)
        if (!L->owed[i - 1].block && L->owed[i - 1].local == local) {
            *flag = L->owed[i - 1].flag;
            return 1;
        }
    return 0;
}

static LowerVar *find_var(Lower *L, const char *cname)
{
    unsigned i;
    if (!cname) return 0;
    for (i = L->var_count; i > 0; --i)
        if (L->vars[i - 1].cname && strcmp(L->vars[i - 1].cname, cname) == 0)
            return &L->vars[i - 1];
    return 0;
}

/* --------------------------------------------------------------- blocks --- */

static FeIrBlock *new_block(Lower *L)
{
    return fe_ir_block(L->m, L->fn);
}

/* A check that must hold. `ok` is a condition; when it is false the program
   stops where it is. `--no-checks` removes the comparison and the branch, not
   just the message, which is the whole point of the flag. */
static void guard(Lower *L, unsigned ok, FeIrTrap reason, unsigned long line)
{
    FeIrBlock *bad = new_block(L);
    FeIrBlock *cont = new_block(L);
    fe_ir_br(L->b, ok, cont->id, bad->id);
    L->b = bad;
    fe_ir_trap(L->b, reason, line);
    L->b = cont;
}

/* A tag says which of the two things a wrapper holds. An optional is one byte
   at the front unless the payload has a spare representation; an error union is
   a two-byte error code, and zero means there is no error. */
static FeIrType tag_type(const FeType *t)
{
    return t && t->kind == FE_TYPE_ERROR_UNION ? FE_IR_I16 : FE_IR_I8;
}

static int uses_niche(const FeType *t)
{
    return t && t->kind == FE_TYPE_OPTIONAL && fe_m7_optional_uses_niche(t->elem);
}

/* Somewhere to build an aggregate that has no home of its own yet. */
static unsigned scratch(Lower *L, const FeType *t, const char *why)
{
    return fe_ir_local(L->m, L->fn, ir_type(t), ir_size(t), ir_align(t), why);
}

/* A slice is a pointer and a length, in that order. Both the compiler and the
   runtime read it this way, so the offsets live here and nowhere else. */
#define SLICE_PTR_OFFSET 0L
#define SLICE_LEN_OFFSET 4L

/* The number of elements an indexable place holds, and where the first element
   is. An array is its own storage; a slice points at someone else's. */
static void indexable_parts(Lower *L, Slot base, const FeType *t,
                            unsigned *data, unsigned *length, FeNode *n)
{
    if (t && t->kind == FE_TYPE_ARRAY) {
        *data = as_address(L, base, n);
        *length = fe_ir_const(L->m, L->b, FE_IR_I32, (long)t->length);
        return;
    }
    if (!base.is_place) { fail(L, "a slice with no place", n); *data = 0; *length = 0; return; }
    *data = fe_ir_load(L->m, L->b, FE_IR_PTR,
                       fe_ir_at_temp(as_address(L, base, n), SLICE_PTR_OFFSET));
    {
        FeIrPlace lp = base.place;
        lp.offset += SLICE_LEN_OFFSET;
        *length = fe_ir_load(L->m, L->b, FE_IR_I32, lp);
    }
}

/* ------------------------------------------------------- error codes ----- */

static void note_error_name(Lower *L, const char *name)
{
    unsigned i;
    unsigned at;
    if (!name || L->error_count >= 256) return;
    for (i = 0; i < L->error_count; ++i)
        if (!strcmp(L->error_names[i], name)) return;
    /* Kept sorted as it is built, so the numbering is the spelling order. */
    at = L->error_count;
    while (at > 0 && strcmp(L->error_names[at - 1], name) > 0) {
        L->error_names[at] = L->error_names[at - 1];
        --at;
    }
    L->error_names[at] = name;
    ++L->error_count;
}

static void collect_error_names(Lower *L, FeNode *n)
{
    FeNode *x;
    if (!n) return;
    /* Allocation reports failure with a name like any other, so it has to be
       in the table even though no source line writes it. */
    if (n->kind == FE_N_CALL && n->a && n->a->kind == FE_N_MEMBER &&
        n->a->a && n->a->a->kind == FE_N_IDENT && n->a->a->text &&
        !strcmp(n->a->a->text, "mem") && n->a->b && n->a->b->text &&
        (!strcmp(n->a->b->text, "create") ||
         !strcmp(n->a->b->text, "alloc_slice")))
        note_error_name(L, "OutOfMemory");
    if (n->kind == FE_N_MEMBER && n->a && n->a->kind == FE_N_IDENT &&
        n->a->text && !strcmp(n->a->text, "error") && n->b && n->b->text)
        note_error_name(L, n->b->text);
    collect_error_names(L, n->a);
    collect_error_names(L, n->b);
    collect_error_names(L, n->c);
    for (x = n->children; x; x = x->next) collect_error_names(L, x);
}

static long error_code(Lower *L, const char *name)
{
    unsigned i;
    for (i = 0; i < L->error_count; ++i)
        if (!strcmp(L->error_names[i], name)) return (long)(i + 1);
    return 0;
}

/* ---------------------------------------------------------- expressions --- */

static FeIrOp binary_op(const char *op, int *is_cmp)
{
    *is_cmp = 0;
    if (!op) return FE_IR_ADD;
    if (!strcmp(op, "+") || !strcmp(op, "+%")) return FE_IR_ADD;
    if (!strcmp(op, "-") || !strcmp(op, "-%")) return FE_IR_SUB;
    if (!strcmp(op, "*") || !strcmp(op, "*%")) return FE_IR_MUL;
    if (!strcmp(op, "/")) return FE_IR_DIV;
    if (!strcmp(op, "%")) return FE_IR_MOD;
    if (!strcmp(op, "&")) return FE_IR_AND;
    if (!strcmp(op, "|")) return FE_IR_OR;
    if (!strcmp(op, "^")) return FE_IR_XOR;
    if (!strcmp(op, "<<")) return FE_IR_SHL;
    if (!strcmp(op, ">>")) return FE_IR_SHR;
    *is_cmp = 1;
    if (!strcmp(op, "==")) return FE_IR_EQ;
    if (!strcmp(op, "!=")) return FE_IR_NE;
    if (!strcmp(op, "<")) return FE_IR_LT;
    if (!strcmp(op, "<=")) return FE_IR_LE;
    if (!strcmp(op, ">")) return FE_IR_GT;
    if (!strcmp(op, ">=")) return FE_IR_GE;
    *is_cmp = 0;
    return FE_IR_ADD;
}

static long literal_value(FeNode *n)
{
    const char *s = n->text;
    long v = 0;
    int neg = 0;
    if (!s) return 0;
    if (!strcmp(s, "true")) return 1;
    if (!strcmp(s, "false")) return 0;
    if (!strcmp(s, "null") || !strcmp(s, "undefined")) return 0;
    if (*s == '\'') {
        /* A character literal; the lexer kept the quotes. */
        if (s[1] == '\\') {
            switch (s[2]) {
            case 'n': return 10;
            case 't': return 9;
            case 'r': return 13;
            case '0': return 0;
            default:  return (long)(unsigned char)s[2];
            }
        }
        return (long)(unsigned char)s[1];
    }
    if (*s == '-') { neg = 1; ++s; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (; *s; ++s) {
            int d = *s >= '0' && *s <= '9' ? *s - '0' :
                    *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
                    *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (d < 0) { if (*s == '_') continue; break; }
            v = v * 16 + d;
        }
    } else {
        for (; *s; ++s) {
            if (*s == '_') continue;
            if (*s < '0' || *s > '9') break;
            v = v * 10 + (*s - '0');
        }
    }
    return neg ? -v : v;
}

/* `and` and `or` do not evaluate the right side unless they have to, so they
   are control flow rather than an operation. */
static Slot lower_logical(Lower *L, FeNode *n, int is_and)
{
    unsigned result = fe_ir_local(L->m, L->fn, FE_IR_I8, 1, 1, "logical");
    FeIrBlock *rhs = new_block(L);
    FeIrBlock *join = new_block(L);
    FeIrBlock *entry = L->b;
    unsigned left;
    unsigned right;
    L->b = entry;
    left = as_value(L, lower_expr(L, n->a), n->a);
    fe_ir_store(L->m, L->b, fe_ir_at_local(result, 0), left, FE_IR_I8);
    if (is_and) fe_ir_br(L->b, left, rhs->id, join->id);
    else fe_ir_br(L->b, left, join->id, rhs->id);
    L->b = rhs;
    right = as_value(L, lower_expr(L, n->b), n->b);
    fe_ir_store(L->m, L->b, fe_ir_at_local(result, 0), right, FE_IR_I8);
    fe_ir_jmp(L->b, join->id);
    L->b = join;
    return slot_place(fe_ir_at_local(result, 0), FE_IR_I8, 1);
}

/* The builtins that are not calls at all: they are a constant, or they stop
   the program. `@print` is expanded separately because it becomes several
   calls rather than one thing. */
static int lower_builtin(Lower *L, FeNode *n, Slot *out)
{
    const char *name = n->text;
    if (!name || name[0] != '@') return 0;
    if (!strcmp(name, "@trap")) {
        fe_ir_trap(L->b, FE_TRAP_EXPLICIT, n->loc.line);
        L->b = new_block(L);
        *out = slot_void();
        return 1;
    }
    if (!strcmp(name, "@unreachable")) {
        fe_ir_trap(L->b, FE_TRAP_UNREACHABLE, n->loc.line);
        L->b = new_block(L);
        *out = slot_void();
        return 1;
    }
    if (!strcmp(name, "@size_of") || !strcmp(name, "@align_of")) {
        FeNode *arg = n->children;
        FeType *t = arg && arg->kind == FE_N_IDENT
            ? fe_type_intern(&L->c->types, arg->text) : 0;
        long v = !strcmp(name, "@size_of") ? (long)ir_size(t)
                                           : (long)ir_align(t);
        *out = slot_value(fe_ir_const(L->m, L->b, FE_IR_I32, v), FE_IR_I32);
        return 1;
    }
    if (!strcmp(name, "@line")) {
        *out = slot_value(fe_ir_const(L->m, L->b, FE_IR_I32,
                                      (long)n->loc.line), FE_IR_I32);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------- mem.* ----- *
 * The allocating intrinsics. They are not ordinary calls: `mem.create` takes a
 * value and gives back an owned pointer to a copy of it, and the result is an
 * error union because the allocation can fail. The runtime does the allocating;
 * everything else about the shape is decided here.
 * -------------------------------------------------------------------------- */

static const char *RT_ALLOC = "fe_rt_alloc";
static const char *RT_FREE = "fe_rt_free";

static int is_mem_call(const FeNode *n, const char *what)
{
    return n && n->a && n->a->kind == FE_N_MEMBER &&
           n->a->a && n->a->a->kind == FE_N_IDENT && n->a->a->text &&
           !strcmp(n->a->a->text, "mem") &&
           n->a->b && n->a->b->text && !strcmp(n->a->b->text, what);
}

/* Build `!^T`: zero and the pointer when the allocation worked, the
   out-of-memory code when it did not. */
static Slot allocation_result(Lower *L, FeNode *n, unsigned pointer)
{
    FeType *t = n->sem_type;
    unsigned local = scratch(L, t, "allocated");
    long payload_at = (long)fe_type_payload_offset(t);
    unsigned zero = fe_ir_const(L->m, L->b, FE_IR_PTR, 0);
    unsigned ok = fe_ir_binary(L->m, L->b, FE_IR_NE, FE_IR_PTR, pointer, zero, 1);
    FeIrBlock *good = new_block(L);
    FeIrBlock *bad = new_block(L);
    FeIrBlock *join = new_block(L);
    fe_ir_br(L->b, ok, good->id, bad->id);
    L->b = good;
    {
        unsigned none = fe_ir_const(L->m, L->b, FE_IR_I16, 0);
        fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), none, FE_IR_I16);
        fe_ir_store(L->m, L->b, fe_ir_at_local(local, payload_at), pointer,
                    FE_IR_PTR);
    }
    fe_ir_jmp(L->b, join->id);
    L->b = bad;
    {
        unsigned code = fe_ir_const(L->m, L->b, FE_IR_I16,
                                    error_code(L, "OutOfMemory"));
        fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), code, FE_IR_I16);
    }
    fe_ir_jmp(L->b, join->id);
    L->b = join;
    return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
}

static int lower_mem(Lower *L, FeNode *n, Slot *out)
{
    unsigned args[2];
    if (is_mem_call(n, "create")) {
        FeNode *arg = n->children;
        FeType *value = arg ? arg->sem_type : 0;
        unsigned size = fe_ir_const(L->m, L->b, FE_IR_I32,
                                    (long)ir_size(value));
        unsigned p;
        Slot v;
        args[0] = size;
        p = fe_ir_call(L->m, L->b, FE_IR_PTR, RT_ALLOC, args, 1);
        /* The value is written through the new pointer, not copied into a
           local first: `create` moves what it was given. */
        v = lower_expr(L, arg);
        store_into(L, fe_ir_at_temp(p, 0), v, arg, ir_size(value));
        *out = allocation_result(L, n, p);
        return 1;
    }
    if (is_mem_call(n, "alloc_slice")) {
        FeNode *type_arg = n->children;
        FeNode *count_arg = type_arg ? type_arg->next : 0;
        FeType *t = n->sem_type;
        /* `!^[]T` -- the payload is an owned slice, a pointer and a length. */
        FeType *owned = t ? t->error_value : 0;
        FeType *slice = owned ? owned->elem : 0;
        FeType *elem = slice ? slice->elem : 0;
        unsigned each = fe_ir_const(L->m, L->b, FE_IR_I32, (long)ir_size(elem));
        unsigned howmany = count_arg
            ? as_value(L, lower_expr(L, count_arg), count_arg)
            : fe_ir_const(L->m, L->b, FE_IR_I32, 0);
        unsigned bytes = fe_ir_binary(L->m, L->b, FE_IR_MUL, FE_IR_I32,
                                      howmany, each, 1);
        unsigned p;
        unsigned local = scratch(L, t, "allocated");
        long payload_at = (long)fe_type_payload_offset(t);
        unsigned zero;
        unsigned ok;
        FeIrBlock *good;
        FeIrBlock *bad;
        FeIrBlock *join;
        args[0] = bytes;
        p = fe_ir_call(L->m, L->b, FE_IR_PTR, RT_ALLOC, args, 1);
        zero = fe_ir_const(L->m, L->b, FE_IR_PTR, 0);
        ok = fe_ir_binary(L->m, L->b, FE_IR_NE, FE_IR_PTR, p, zero, 1);
        good = new_block(L);
        bad = new_block(L);
        join = new_block(L);
        fe_ir_br(L->b, ok, good->id, bad->id);
        L->b = good;
        {
            unsigned none = fe_ir_const(L->m, L->b, FE_IR_I16, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), none, FE_IR_I16);
            fe_ir_store(L->m, L->b,
                        fe_ir_at_local(local, payload_at + SLICE_PTR_OFFSET),
                        p, FE_IR_PTR);
            fe_ir_store(L->m, L->b,
                        fe_ir_at_local(local, payload_at + SLICE_LEN_OFFSET),
                        howmany, FE_IR_I32);
        }
        fe_ir_jmp(L->b, join->id);
        L->b = bad;
        {
            unsigned code = fe_ir_const(L->m, L->b, FE_IR_I16,
                                        error_code(L, "OutOfMemory"));
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), code, FE_IR_I16);
        }
        fe_ir_jmp(L->b, join->id);
        L->b = join;
        *out = slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
        return 1;
    }
    if (is_mem_call(n, "destroy")) {
        FeNode *arg = n->children;
        Slot p = lower_expr(L, arg);
        /* An owned slice is a pointer and a length; what was allocated is the
           pointer. */
        if (p.type == FE_IR_MEM) {
            FeIrPlace at = p.place;
            at.offset += SLICE_PTR_OFFSET;
            args[0] = fe_ir_load(L->m, L->b, FE_IR_PTR, at);
        } else {
            args[0] = as_value(L, p, arg);
        }
        fe_ir_call(L->m, L->b, FE_IR_VOID, RT_FREE, args, 1);
        *out = slot_void();
        return 1;
    }
    if (is_mem_call(n, "replace")) {
        /* Read what is there, put the new value in its place, hand back the
           old one. This is how a value is taken out of a field without ever
           leaving the field uninitialised (SPEC 5 R7). */
        FeNode *dst = n->children;
        FeNode *value = dst ? dst->next : 0;
        FeType *t = n->sem_type;
        unsigned target = as_value(L, lower_expr(L, dst), dst);
        unsigned old = scratch(L, t, "replaced");
        Slot fresh;
        fe_ir_copy(L->m, L->b, fe_ir_at_local(old, 0), fe_ir_at_temp(target, 0),
                   ir_size(t));
        fresh = lower_expr(L, value);
        store_into(L, fe_ir_at_temp(target, 0), fresh, value, ir_size(t));
        *out = slot_place(fe_ir_at_local(old, 0), ir_type(t), ir_size(t));
        return 1;
    }
    return 0;
}

static Slot lower_call(Lower *L, FeNode *n)
{
    unsigned args[16];
    unsigned count = 0;
    FeNode *arg = n->children;
    FeType *ret = n->sem_type;
    FeIrType rt = ir_type(ret);
    unsigned result_local = 0;
    const char *callee = n->a && n->a->cname ? n->a->cname :
                         (n->sem_decl && n->sem_decl->cname ?
                          n->sem_decl->cname : 0);
    {
        Slot built;
        if (lower_builtin(L, n, &built)) return built;
        if (lower_mem(L, n, &built)) return built;
    }
    if (!callee) { fail(L, "a call with no target", n); return slot_void(); }
    /* An aggregate result is written through a hidden first argument. */
    if (rt == FE_IR_MEM) {
        result_local = fe_ir_local(L->m, L->fn, FE_IR_MEM, ir_size(ret),
                                   ir_align(ret), "result");
        args[count++] = fe_ir_addr(L->m, L->b, fe_ir_at_local(result_local, 0));
    }
    /* A method call passes what it was reached through as its first argument.
       `self: Self` and `self: &Self` are the same thing here: the address of
       the receiver, because an aggregate never travels in a register. */
    if (n->a && n->a->kind == FE_N_MEMBER && n->sem_decl) {
        FeNode *first = n->sem_decl->a ? n->sem_decl->a->children : 0;
        if (first && first->text && !strcmp(first->text, "self")) {
            FeType *rt = n->a->a ? n->a->a->sem_type : 0;
            Slot recv = lower_expr(L, n->a->a);
            /* A receiver that is already a reference or an owner is a pointer
               already; taking its address would pass a pointer to the
               pointer. */
            if (rt && (rt->kind == FE_TYPE_REF ||
                       (rt->kind == FE_TYPE_OWNED && ir_type(rt) == FE_IR_PTR)))
                args[count++] = as_value(L, recv, n->a->a);
            else
                args[count++] = recv.is_place ? as_address(L, recv, n->a->a)
                                              : recv.temp;
        }
    }
    /* A generic call passes its type arguments first. They were consumed when
       the instance was chosen and carry no value, so they are not passed. */
    {
        FeNode *p;
        for (p = n->sem_decl && n->sem_decl->a ? n->sem_decl->a->children : 0;
             p && arg; p = p->next) {
            if (!(p->flags & FE_NODE_COMPTIME)) break;
            arg = arg->next;
        }
    }
    for (; arg; arg = arg->next) {
        Slot a = lower_expr(L, arg);
        if (count >= 16) { fail(L, "too many arguments", n); break; }
        args[count++] = a.type == FE_IR_MEM ? as_address(L, a, arg)
                                            : as_value(L, a, arg);
    }
    if (rt == FE_IR_MEM) {
        fe_ir_call(L->m, L->b, FE_IR_VOID, callee, args, count);
        return slot_place(fe_ir_at_local(result_local, 0), FE_IR_MEM,
                          ir_size(ret));
    }
    if (rt == FE_IR_VOID) {
        fe_ir_call(L->m, L->b, FE_IR_VOID, callee, args, count);
        return slot_void();
    }
    return slot_value(fe_ir_call(L->m, L->b, rt, callee, args, count), rt);
}

static Slot lower_expr_core(Lower *L, FeNode *n);

/* Every expression may be standing where a wrapper is expected, so the wrap is
   applied once, here, rather than at each place that could need it. */
static Slot lower_expr(Lower *L, FeNode *n)
{
    Slot v;
    if (!n || L->failed) return slot_void();
    v = lower_expr_core(L, n);
    /* The checker marked the uses that hand ownership away. Where one names a
       local we track, the value is no longer ours to release. */
    if ((n->flags & FE_OWN_NODE_CONSUMED) && n->kind == FE_N_IDENT) {
        LowerVar *var = find_var(L, n->cname);
        unsigned flag;
        if (var && release_flag(L, var->local, &flag)) {
            unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I8, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(flag, 0), zero, FE_IR_I8);
        }
    }
    return n->sem_context ? wrap_context(L, v, n) : v;
}

static Slot lower_expr_core(Lower *L, FeNode *n)
{
    FeType *t;
    FeIrType it;
    if (!n || L->failed) return slot_void();
    t = n->sem_type;
    it = ir_type(t);
    switch (n->kind) {
    case FE_N_LITERAL:
        if (n->text && n->text[0] == '"') {
            /* The bytes live in the image; the value is a pointer to them and
               how many there are. The lexer keeps the quotes and the escapes,
               so this is where `
` becomes one byte. */
            char text[1024];
            unsigned long raw = strlen(n->text);
            unsigned long len = 0;
            unsigned long i;
            const char *label;
            unsigned local;
            unsigned p;
            unsigned c;
            if (raw >= 2) raw -= 2;
            for (i = 0; i < raw && len + 1 < sizeof text; ++i) {
                char ch = n->text[1 + i];
                if (ch == 92 && i + 1 < raw) {          /* a backslash */
                    ++i;
                    switch (n->text[1 + i]) {
                    case 'n': ch = 10; break;
                    case 't': ch = 9; break;
                    case 'r': ch = 13; break;
                    case '0': ch = 0; break;
                    default:  ch = n->text[1 + i]; break;
                    }
                }
                text[len++] = ch;
            }
            label = fe_ir_string(L->m, text, len);
            if (!label) { fail(L, "a string literal", n); return slot_void(); }
            local = scratch(L, t, "text");
            p = fe_ir_addr(L->m, L->b, fe_ir_at_global(label, 0));
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, SLICE_PTR_OFFSET), p,
                        FE_IR_PTR);
            c = fe_ir_const(L->m, L->b, FE_IR_I32, (long)len);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, SLICE_LEN_OFFSET), c,
                        FE_IR_I32);
            return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
        }
        return slot_value(fe_ir_const(L->m, L->b,
                                      it == FE_IR_VOID ? FE_IR_I32 : it,
                                      literal_value(n)),
                          it == FE_IR_VOID ? FE_IR_I32 : it);
    case FE_N_IDENT: {
        LowerVar *var = find_var(L, n->cname);
        if (var) {
            if (var->by_address) {
                unsigned p = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                        fe_ir_at_local(var->local, 0));
                return slot_place(fe_ir_at_temp(p, 0), it, ir_size(t));
            }
            return slot_place(fe_ir_at_local(var->local, 0), it, ir_size(t));
        }
        if (n->cname)
            return slot_place(fe_ir_at_global(n->cname, 0), it, ir_size(t));
        fail(L, "an unresolved name", n);
        return slot_void();
    }
    case FE_N_BINARY: {
        int is_cmp = 0;
        FeIrOp op;
        unsigned a;
        unsigned b;
        FeIrType operand;
        if (n->text && !strcmp(n->text, "orelse")) return lower_lazy(L, n, 0);
        if (n->text && !strcmp(n->text, "catch")) return lower_lazy(L, n, 1);
        if (n->text && (!strcmp(n->text, "and") || !strcmp(n->text, "or")))
            return lower_logical(L, n, !strcmp(n->text, "and"));
        op = binary_op(n->text, &is_cmp);
        operand = ir_type(n->a ? n->a->sem_type : 0);
        if (operand == FE_IR_VOID || operand == FE_IR_MEM) operand = FE_IR_I32;
        a = as_value(L, lower_expr(L, n->a), n->a);
        b = as_value(L, lower_expr(L, n->b), n->b);
        return slot_value(fe_ir_binary(L->m, L->b, op, operand, a, b,
                                       type_is_unsigned(n->a ? n->a->sem_type
                                                             : 0)),
                          is_cmp ? FE_IR_I8 : operand);
    }
    case FE_N_UNARY:
        if (n->text && !strcmp(n->text, "try")) return lower_try(L, n);
        if (n->text && !strcmp(n->text, "-")) {
            unsigned zero = fe_ir_const(L->m, L->b, it, 0);
            unsigned v = as_value(L, lower_expr(L, n->a), n->a);
            return slot_value(fe_ir_binary(L->m, L->b, FE_IR_SUB, it, zero, v,
                                           0), it);
        }
        if (n->text && !strcmp(n->text, "not")) {
            unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I8, 0);
            unsigned v = as_value(L, lower_expr(L, n->a), n->a);
            return slot_value(fe_ir_binary(L->m, L->b, FE_IR_EQ, FE_IR_I8, v,
                                           zero, 0), FE_IR_I8);
        }
        if (n->text && (!strcmp(n->text, "&") || !strcmp(n->text, "&mut"))) {
            Slot inner = lower_expr(L, n->a);
            return slot_value(as_address(L, inner, n->a), FE_IR_PTR);
        }
        fail(L, "this unary operator", n);
        return slot_void();
    case FE_N_MEMBER:
        /* A payload-free variant used as a value is just its tag. */
        if (t && t->kind == FE_TYPE_ENUM && !enum_has_payload(t) &&
            n->b && n->b->text) {
            FeVariantType *v = fe_type_variant(t, n->b->text);
            if (v)
                return slot_value(fe_ir_const(L->m, L->b, ir_type(t),
                                              (long)v->tag), ir_type(t));
        }
        /* `error.Name` is a member of the open default set: a code, and
           nothing to look up. */
        if (n->a && n->a->kind == FE_N_IDENT && n->a->text &&
            !strcmp(n->a->text, "error") && n->b && n->b->text)
            return slot_value(fe_ir_const(L->m, L->b, FE_IR_I16,
                                          error_code(L, n->b->text)),
                              FE_IR_I16);
        /* `.?` is the payload of an optional the checker already proved is
           there. */
        if (n->text && !strcmp(n->text, ".?")) {
            FeType *bt = n->a ? n->a->sem_type : 0;
            return wrapper_payload(L, lower_expr(L, n->a), bt);
        }
        /* `p.^` reads through a pointer -- except for an owned slice, whose
           pointer and length are the value itself, so there is nothing to
           step through. */
        if (n->text && !strcmp(n->text, ".^")) {
            Slot base = lower_expr(L, n->a);
            unsigned p;
            if (base.type == FE_IR_MEM)
                return slot_place(base.place, it, ir_size(t));
            p = as_value(L, base, n->a);
            return slot_place(fe_ir_at_temp(p, 0), it, ir_size(t));
        }
        /* `.n` is how many elements there are, which an array knows at
           compile time and a slice carries beside its pointer. */
        if (n->b && n->b->text && !strcmp(n->b->text, "n")) {
            FeType *bt = n->a ? n->a->sem_type : 0;
            Slot base;
            if (bt && bt->kind == FE_TYPE_ARRAY)
                return slot_value(fe_ir_const(L->m, L->b, FE_IR_I32,
                                              (long)bt->length), FE_IR_I32);
            base = lower_expr(L, n->a);
            if (!base.is_place) { fail(L, "a length of a temporary", n); return slot_void(); }
            base.place.offset += SLICE_LEN_OFFSET;
            return slot_place(base.place, FE_IR_I32, 4);
        }
        /* A field is a constant offset from the base. */
        {
            FeType *base = n->a ? n->a->sem_type : 0;
            FeFieldType *field;
            Slot b;
            if (base && (base->kind == FE_TYPE_REF ||
                         base->kind == FE_TYPE_OWNED)) base = base->elem;
            field = fe_type_field(base, n->b && n->b->text ? n->b->text : "");
            if (!field) { fail(L, "an unresolved field", n); return slot_void(); }
            b = lower_expr(L, n->a);
            if (n->a->sem_type && (n->a->sem_type->kind == FE_TYPE_REF ||
                                   n->a->sem_type->kind == FE_TYPE_OWNED)) {
                unsigned p = as_value(L, b, n->a);
                return slot_place(fe_ir_at_temp(p, (long)field->offset), it,
                                  ir_size(t));
            }
            if (!b.is_place) { fail(L, "a field of a temporary", n); return slot_void(); }
            b.place.offset += (long)field->offset;
            return slot_place(b.place, it, ir_size(t));
        }
    case FE_N_INDEX: {
        FeType *bt = n->a ? n->a->sem_type : 0;
        FeType *elem = bt ? bt->elem : 0;
        Slot base;
        unsigned data;
        unsigned length;
        unsigned index;
        unsigned scale;
        unsigned offset;
        unsigned addr;
        if (n->flags & FE_NODE_SLICE) return lower_slice(L, n);
        base = lower_expr(L, n->a);
        indexable_parts(L, base, bt, &data, &length, n);
        index = as_value(L, lower_expr(L, n->b), n->b);
        if (!L->c->no_checks) {
            unsigned ok = fe_ir_binary(L->m, L->b, FE_IR_LT, FE_IR_I32,
                                       index, length, 1);
            guard(L, ok, FE_TRAP_BOUNDS, n->loc.line);
        }
        scale = fe_ir_const(L->m, L->b, FE_IR_I32, (long)ir_size(elem));
        offset = fe_ir_binary(L->m, L->b, FE_IR_MUL, FE_IR_I32, index, scale, 1);
        addr = fe_ir_binary(L->m, L->b, FE_IR_ADD, FE_IR_PTR, data, offset, 1);
        return slot_place(fe_ir_at_temp(addr, 0), ir_type(elem), ir_size(elem));
    }
    case FE_N_ARRAY_INIT: {
        unsigned local = scratch(L, t, "array");
        FeType *elem = t ? t->elem : 0;
        unsigned long step = ir_size(elem);
        long at = 0;
        FeNode *x;
        for (x = n->children; x; x = x->next) {
            Slot v = lower_expr(L, x);
            store_into(L, fe_ir_at_local(local, at), v, x, step);
            at += (long)step;
        }
        return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
    }
    case FE_N_STRUCT_INIT: {
        unsigned local = scratch(L, t, "struct");
        FeNode *f;
        for (f = n->children; f; f = f->next) {
            FeFieldType *field;
            Slot v;
            if (f->kind != FE_N_FIELD) continue;
            field = fe_type_field(t, f->text);
            if (!field) { fail(L, "an unresolved field", f); return slot_void(); }
            v = lower_expr(L, f->a);
            store_into(L, fe_ir_at_local(local, (long)field->offset), v, f,
                       ir_size(field->type));
        }
        return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
    }
    case FE_N_CALL:
        return lower_call(L, n);
    case FE_N_TYPE:
        /* `x as T`: the operand is `a` and the target type is the node's own.
           Between integers this only changes how wide the value is and whether
           the top bits repeat the sign. */
        if (n->a) {
            FeType *from = n->a->sem_type;
            unsigned v = as_value(L, lower_expr(L, n->a), n->a);
            if (ir_type(from) == it) return slot_value(v, it);
            return slot_value(fe_ir_cast(L->m, L->b, ir_type(from), it, v,
                                         type_is_unsigned(from)), it);
        }
        fail(L, "this type expression", n);
        return slot_void();
    case FE_N_EXPR:
        return lower_expr(L, n->a);
    default:
        fail(L, "this expression", n);
        return slot_void();
    }
}

/* The link name of the `drop` method for this type, found through the instance
   the checker recorded. */
static const char *drop_name(Lower *L, const FeType *t)
{
    unsigned i;
    FeNode *method = 0;
    if (!t || !t->decl_node) return 0;
    for (method = t->decl_node->children; method; method = method->next)
        if (method->kind == FE_N_FN && method->text &&
            !strcmp(method->text, "drop")) break;
    if (!method) return 0;
    for (i = 0; i < L->c->instance_count; ++i)
        if (L->c->instances[i].decl == method &&
            L->c->instances[i].owner == t)
            return L->c->instances[i].cname;
    return method->cname;
}

/* Settle what a scope owes, most recent first. A `return` in the middle of a
   function still owes everything, so every exit path calls this. */
static void run_deferred(Lower *L, unsigned from)
{
    unsigned i;
    for (i = L->owed_count; i > from; --i) {
        if (L->owed[i - 1].block) {
            lower_stmt(L, L->owed[i - 1].block);
            continue;
        }
        {
            /* Release only where the value is still here. */
            unsigned live = fe_ir_load(L->m, L->b, FE_IR_I8,
                                       fe_ir_at_local(L->owed[i - 1].flag, 0));
            FeIrBlock *doit = new_block(L);
            FeIrBlock *skip = new_block(L);
            unsigned args[1];
            FeType *t = L->owed[i - 1].type;
            fe_ir_br(L->b, live, doit->id, skip->id);
            L->b = doit;
            if (t && t->kind == FE_TYPE_OWNED && t->elem &&
                t->elem->kind == FE_TYPE_SLICE) {
                FeIrPlace at = fe_ir_at_local(L->owed[i - 1].local,
                                              SLICE_PTR_OFFSET);
                args[0] = fe_ir_load(L->m, L->b, FE_IR_PTR, at);
            } else {
                args[0] = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                     fe_ir_at_local(L->owed[i - 1].local, 0));
            }
            if (t && t->has_drop) {
                /* A type that says how to let go of itself is asked to; the
                   name is the one its instance was given. */
                const char *how = drop_name(L, t);
                args[0] = fe_ir_addr(L->m, L->b,
                                     fe_ir_at_local(L->owed[i - 1].local, 0));
                if (how) fe_ir_call(L->m, L->b, FE_IR_VOID, how, args, 1);
            } else {
                fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_free", args, 1);
            }
            fe_ir_jmp(L->b, skip->id);
            L->b = skip;
        }
    }
}

/* ------------------------------------------------------- wrappers -------- *
 * An optional is a tag and a payload; an error union is an error code and a
 * payload, where a code of zero means there is no error. Both are memory, and
 * both are built the same way: write the tag, then write the value after it.
 * -------------------------------------------------------------------------- */

static Slot wrap_context(Lower *L, Slot v, FeNode *n)
{
    FeType *want = n->sem_context;
    unsigned local;
    long payload_at;
    if (!want) return v;
    local = scratch(L, want, "wrapped");
    payload_at = (long)fe_type_payload_offset(want);
    if (want->kind == FE_TYPE_OPTIONAL) {
        if (fe_m7_is_null(n)) {
            /* A payload with a spare representation uses it for "nothing"
               instead of carrying a separate tag. */
            unsigned z = fe_ir_const(L->m, L->b,
                                     uses_niche(want) ? FE_IR_PTR : FE_IR_I8, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), z,
                        uses_niche(want) ? FE_IR_PTR : FE_IR_I8);
            return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(want));
        }
        if (!uses_niche(want)) {
            unsigned one = fe_ir_const(L->m, L->b, FE_IR_I8, 1);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), one, FE_IR_I8);
        }
        store_into(L, fe_ir_at_local(local, payload_at), v, n,
                   ir_size(want->elem));
        return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(want));
    }
    if (want->kind == FE_TYPE_ERROR_UNION) {
        FeType *value_type = want->error_value;
        if (n->sem_type && n->sem_type->is_error) {
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0),
                        as_value(L, v, n), FE_IR_I16);
        } else {
            unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I16, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), zero, FE_IR_I16);
            if (value_type && value_type->kind != FE_TYPE_VOID)
                store_into(L, fe_ir_at_local(local, payload_at), v, n,
                           ir_size(value_type));
        }
        return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(want));
    }
    return v;
}

/* The tag of a wrapper that is already in memory. */
static unsigned wrapper_tag(Lower *L, Slot w, const FeType *t, FeNode *n)
{
    FeIrPlace p;
    if (!w.is_place) { fail(L, "a wrapper with no place", n); return 0; }
    p = w.place;
    if (uses_niche(t)) return fe_ir_load(L->m, L->b, FE_IR_PTR, p);
    return fe_ir_load(L->m, L->b, tag_type(t), p);
}

static Slot wrapper_payload(Lower *L, Slot w, const FeType *t)
{
    FeType *payload = t ? (t->kind == FE_TYPE_ERROR_UNION ? t->error_value
                                                          : t->elem) : 0;
    FeIrPlace p = w.place;
    (void)L;
    p.offset += (long)fe_type_payload_offset(t);
    return slot_place(p, ir_type(payload), ir_size(payload));
}

/* Leave the function with this error code, after the deferred blocks. */
static void return_error(Lower *L, unsigned err, FeNode *n)
{
    FeType *ret = L->ret_type;
    unsigned local = scratch(L, ret, "failure");
    fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), err, FE_IR_I16);
    run_deferred(L, 0);
    if (L->fn->returns_by_address) {
        unsigned dst = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                  fe_ir_at_local(L->ret_local, 0));
        fe_ir_copy(L->m, L->b, fe_ir_at_temp(dst, 0), fe_ir_at_local(local, 0),
                   ir_size(ret));
        fe_ir_ret(L->b, 0, 0);
        return;
    }
    fe_ir_ret(L->b, fe_ir_load(L->m, L->b, ir_type(ret),
                               fe_ir_at_local(local, 0)), 1);
    (void)n;
}

/* `try e` -- if e failed, leave with its error; otherwise the value. */
static Slot lower_try(Lower *L, FeNode *n)
{
    FeType *t = n->a ? n->a->sem_type : 0;
    Slot e = lower_expr(L, n->a);
    unsigned err = wrapper_tag(L, e, t, n);
    unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I16, 0);
    unsigned ok = fe_ir_binary(L->m, L->b, FE_IR_EQ, FE_IR_I16, err, zero, 1);
    FeIrBlock *bad = new_block(L);
    FeIrBlock *good = new_block(L);
    fe_ir_br(L->b, ok, good->id, bad->id);
    L->b = bad;
    return_error(L, err, n);
    L->b = good;
    return wrapper_payload(L, e, t);
}

/* `e orelse d` and `e catch d` both mean "the value, or that instead". The
   right-hand side is only evaluated when it is needed, so it is a branch. */
static Slot lower_lazy(Lower *L, FeNode *n, int is_catch)
{
    FeType *t = n->a ? n->a->sem_type : 0;
    FeType *payload = t ? (is_catch ? t->error_value : t->elem) : 0;
    Slot e;
    unsigned tag;
    unsigned zero;
    unsigned ok;
    unsigned result;
    FeIrBlock *other;
    FeIrBlock *join;
    FeIrBlock *have;
    e = lower_expr(L, n->a);
    tag = wrapper_tag(L, e, t, n);
    zero = fe_ir_const(L->m, L->b, is_catch || uses_niche(t) ? FE_IR_PTR
                                                             : FE_IR_I8, 0);
    /* An error union is fine when its code is zero; an optional is fine when
       its tag is not. */
    ok = fe_ir_binary(L->m, L->b, is_catch ? FE_IR_EQ : FE_IR_NE,
                      is_catch ? FE_IR_I16 : (uses_niche(t) ? FE_IR_PTR
                                                            : FE_IR_I8),
                      tag, zero, 1);
    result = scratch(L, payload, "result");
    have = new_block(L);
    other = new_block(L);
    join = new_block(L);
    fe_ir_br(L->b, ok, have->id, other->id);
    L->b = have;
    store_into(L, fe_ir_at_local(result, 0), wrapper_payload(L, e, t), n,
               ir_size(payload));
    fe_ir_jmp(L->b, join->id);
    L->b = other;
    if (is_catch && n->c) {
        /* The block form handles the error and must not fall through with a
           value, so whatever it leaves behind is what the checker allowed. */
        lower_stmt(L, n->c);
    } else {
        Slot d = lower_expr(L, n->b);
        store_into(L, fe_ir_at_local(result, 0), d, n->b, ir_size(payload));
    }
    fe_ir_jmp(L->b, join->id);
    L->b = join;
    return slot_place(fe_ir_at_local(result, 0), ir_type(payload),
                      ir_size(payload));
}

/* ----------------------------------------------------------- statements --- */

static void store_into(Lower *L, FeIrPlace dst, Slot value, FeNode *n,
                       unsigned long size)
{
    if (value.type == FE_IR_MEM) {
        if (!value.is_place) { fail(L, "an aggregate value", n); return; }
        fe_ir_copy(L->m, L->b, dst, value.place, size);
        return;
    }
    fe_ir_store(L->m, L->b, dst, as_value(L, value, n), value.type);
}

static void lower_return(Lower *L, FeNode *n)
{
    Slot v;
    if (!n->a) {
        /* A bare return from a `!void` function still has to say that nothing
           went wrong. */
        if (L->ret_type && L->ret_type->kind == FE_TYPE_ERROR_UNION) {
            unsigned local = scratch(L, L->ret_type, "success");
            unsigned none = fe_ir_const(L->m, L->b, FE_IR_I16, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), none, FE_IR_I16);
            run_deferred(L, 0);
            if (L->fn->returns_by_address) {
                unsigned dst = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                          fe_ir_at_local(L->ret_local, 0));
                fe_ir_copy(L->m, L->b, fe_ir_at_temp(dst, 0),
                           fe_ir_at_local(local, 0), ir_size(L->ret_type));
                fe_ir_ret(L->b, 0, 0);
                return;
            }
            fe_ir_ret(L->b, fe_ir_load(L->m, L->b, ir_type(L->ret_type),
                                       fe_ir_at_local(local, 0)), 1);
            return;
        }
        run_deferred(L, 0);
        fe_ir_ret(L->b, 0, 0);
        return;
    }
    /* The value is computed before the deferred blocks run, because they may
       destroy what it was read from. */
    v = lower_expr(L, n->a);
    if (v.type != FE_IR_MEM && v.is_place)
        v = slot_value(as_value(L, v, n->a), v.type);
    run_deferred(L, 0);
    if (L->fn->returns_by_address) {
        unsigned dst = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                  fe_ir_at_local(L->ret_local, 0));
        store_into(L, fe_ir_at_temp(dst, 0), v, n, ir_size(L->ret_type));
        fe_ir_ret(L->b, 0, 0);
        return;
    }
    fe_ir_ret(L->b, as_value(L, v, n->a), 1);
}

static void lower_if(Lower *L, FeNode *n)
{
    FeIrBlock *then_b = new_block(L);
    FeIrBlock *else_b = n->c ? new_block(L) : 0;
    FeIrBlock *join = new_block(L);
    unsigned cond = as_value(L, lower_expr(L, n->a), n->a);
    fe_ir_br(L->b, cond, then_b->id, else_b ? else_b->id : join->id);
    L->b = then_b;
    lower_stmt(L, n->b);
    fe_ir_jmp(L->b, join->id);
    if (else_b) {
        L->b = else_b;
        lower_stmt(L, n->c);
        fe_ir_jmp(L->b, join->id);
    }
    L->b = join;
}

static void lower_while(Lower *L, FeNode *n)
{
    FeIrBlock *head = new_block(L);
    FeIrBlock *body = new_block(L);
    FeIrBlock *done = new_block(L);
    unsigned cond;
    fe_ir_jmp(L->b, head->id);
    L->b = head;
    cond = as_value(L, lower_expr(L, n->a), n->a);
    fe_ir_br(L->b, cond, body->id, done->id);
    if (L->loop_depth < 32) {
        L->break_target[L->loop_depth] = done->id;
        L->continue_target[L->loop_depth] = head->id;
        ++L->loop_depth;
    }
    L->b = body;
    lower_stmt(L, n->b);
    fe_ir_jmp(L->b, head->id);
    if (L->loop_depth) --L->loop_depth;
    L->b = done;
}

/* `x[a..b]` makes a pointer and a length out of part of something indexable.
   Both ends are checked -- against each other and against what is there --
   before the pointer is formed. An empty slice of a valid range is fine; one
   that starts past its end is not. */
static Slot lower_slice(Lower *L, FeNode *n)
{
    FeType *bt = n->a ? n->a->sem_type : 0;
    FeType *elem = bt ? bt->elem : 0;
    FeType *t = n->sem_type;
    Slot base = lower_expr(L, n->a);
    unsigned data;
    unsigned length;
    unsigned from;
    unsigned to;
    unsigned local;
    unsigned scale;
    unsigned off;
    unsigned at;
    unsigned count;
    indexable_parts(L, base, bt, &data, &length, n);
    from = n->b ? as_value(L, lower_expr(L, n->b), n->b)
                : fe_ir_const(L->m, L->b, FE_IR_I32, 0);
    to = n->c ? as_value(L, lower_expr(L, n->c), n->c) : length;
    if (!L->c->no_checks) {
        unsigned ordered = fe_ir_binary(L->m, L->b, FE_IR_LE, FE_IR_I32,
                                        from, to, 1);
        unsigned within;
        guard(L, ordered, FE_TRAP_BOUNDS, n->loc.line);
        within = fe_ir_binary(L->m, L->b, FE_IR_LE, FE_IR_I32, to, length, 1);
        guard(L, within, FE_TRAP_BOUNDS, n->loc.line);
    }
    scale = fe_ir_const(L->m, L->b, FE_IR_I32, (long)ir_size(elem));
    off = fe_ir_binary(L->m, L->b, FE_IR_MUL, FE_IR_I32, from, scale, 1);
    at = fe_ir_binary(L->m, L->b, FE_IR_ADD, FE_IR_PTR, data, off, 1);
    count = fe_ir_binary(L->m, L->b, FE_IR_SUB, FE_IR_I32, to, from, 1);
    local = scratch(L, t, "slice");
    fe_ir_store(L->m, L->b, fe_ir_at_local(local, SLICE_PTR_OFFSET), at,
                FE_IR_PTR);
    fe_ir_store(L->m, L->b, fe_ir_at_local(local, SLICE_LEN_OFFSET), count,
                FE_IR_I32);
    return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
}

/* Three shapes share the keyword.

     for i in a..b { }        counts
     for x in thing { }       walks, binding a reference to each element
     for i, x in thing { }    walks, binding the position as well

   The count is read once before the body, so a thing that grows underneath the
   loop cannot walk past what was measured. The element binding is a reference
   (`x.^` reads it), which is what lets a loop write back into the thing. */
static void lower_for(Lower *L, FeNode *n)
{
    FeIrBlock *head;
    FeIrBlock *body;
    FeIrBlock *step;
    FeIrBlock *done;
    unsigned counter;
    unsigned limit;

    if (n->c) {
        /* The counting form: the variable is the count itself. */
        unsigned from = as_value(L, lower_expr(L, n->a), n->a);
        unsigned to;
        counter = declare_var(L, n->cname, 0, n->text);
        L->fn->locals[counter].type = FE_IR_I32;
        L->fn->locals[counter].size = 4;
        L->fn->locals[counter].align = 4;
        fe_ir_store(L->m, L->b, fe_ir_at_local(counter, 0), from, FE_IR_I32);
        to = as_value(L, lower_expr(L, n->c), n->c);
        limit = fe_ir_local(L->m, L->fn, FE_IR_I32, 4, 4, "limit");
        fe_ir_store(L->m, L->b, fe_ir_at_local(limit, 0), to, FE_IR_I32);
        head = new_block(L);
        body = new_block(L);
        step = new_block(L);
        done = new_block(L);
        fe_ir_jmp(L->b, head->id);
        L->b = head;
        {
            unsigned i = fe_ir_load(L->m, L->b, FE_IR_I32,
                                    fe_ir_at_local(counter, 0));
            unsigned e = fe_ir_load(L->m, L->b, FE_IR_I32,
                                    fe_ir_at_local(limit, 0));
            unsigned more = fe_ir_binary(L->m, L->b, FE_IR_LT, FE_IR_I32, i, e, 1);
            fe_ir_br(L->b, more, body->id, done->id);
        }
    } else {
        FeType *bt = n->a ? n->a->sem_type : 0;
        FeType *elem = bt ? bt->elem : 0;
        Slot base = lower_expr(L, n->a);
        unsigned data;
        unsigned length;
        unsigned data_local;
        unsigned item;
        indexable_parts(L, base, bt, &data, &length, n);
        data_local = fe_ir_local(L->m, L->fn, FE_IR_PTR, 4, 4, "data");
        fe_ir_store(L->m, L->b, fe_ir_at_local(data_local, 0), data, FE_IR_PTR);
        limit = fe_ir_local(L->m, L->fn, FE_IR_I32, 4, 4, "count");
        fe_ir_store(L->m, L->b, fe_ir_at_local(limit, 0), length, FE_IR_I32);
        /* With two names the first is the position and the second the element;
           with one it is the element. */
        counter = fe_ir_local(L->m, L->fn, FE_IR_I32, 4, 4, "index");
        if (n->aux_cname) {
            L->vars[L->var_count].cname = n->cname;
            L->vars[L->var_count].local = counter;
            L->vars[L->var_count].by_address = 0;
            if (L->var_count < LOWER_MAX_LOCALS) ++L->var_count;
            item = fe_ir_local(L->m, L->fn, FE_IR_PTR, 4, 4, n->aux_text);
            L->vars[L->var_count].cname = n->aux_cname;
            L->vars[L->var_count].local = item;
            L->vars[L->var_count].by_address = 0;
            if (L->var_count < LOWER_MAX_LOCALS) ++L->var_count;
        } else {
            item = fe_ir_local(L->m, L->fn, FE_IR_PTR, 4, 4, n->text);
            L->vars[L->var_count].cname = n->cname;
            L->vars[L->var_count].local = item;
            L->vars[L->var_count].by_address = 0;
            if (L->var_count < LOWER_MAX_LOCALS) ++L->var_count;
        }
        {
            unsigned zero = fe_ir_const(L->m, L->b, FE_IR_I32, 0);
            fe_ir_store(L->m, L->b, fe_ir_at_local(counter, 0), zero, FE_IR_I32);
        }
        head = new_block(L);
        body = new_block(L);
        step = new_block(L);
        done = new_block(L);
        fe_ir_jmp(L->b, head->id);
        L->b = head;
        {
            unsigned i = fe_ir_load(L->m, L->b, FE_IR_I32,
                                    fe_ir_at_local(counter, 0));
            unsigned e = fe_ir_load(L->m, L->b, FE_IR_I32,
                                    fe_ir_at_local(limit, 0));
            unsigned more = fe_ir_binary(L->m, L->b, FE_IR_LT, FE_IR_I32, i, e, 1);
            fe_ir_br(L->b, more, body->id, done->id);
        }
        L->b = body;
        {
            unsigned i = fe_ir_load(L->m, L->b, FE_IR_I32,
                                    fe_ir_at_local(counter, 0));
            unsigned scale = fe_ir_const(L->m, L->b, FE_IR_I32,
                                         (long)ir_size(elem));
            unsigned off = fe_ir_binary(L->m, L->b, FE_IR_MUL, FE_IR_I32, i,
                                        scale, 1);
            unsigned p = fe_ir_load(L->m, L->b, FE_IR_PTR,
                                    fe_ir_at_local(data_local, 0));
            unsigned at = fe_ir_binary(L->m, L->b, FE_IR_ADD, FE_IR_PTR, p,
                                       off, 1);
            fe_ir_store(L->m, L->b, fe_ir_at_local(item, 0), at, FE_IR_PTR);
        }
        L->b = head;
    }

    if (L->loop_depth < 32) {
        L->break_target[L->loop_depth] = done->id;
        L->continue_target[L->loop_depth] = step->id;
        ++L->loop_depth;
    }
    L->b = body;
    lower_stmt(L, n->b);
    fe_ir_jmp(L->b, step->id);
    L->b = step;
    {
        unsigned i = fe_ir_load(L->m, L->b, FE_IR_I32,
                                fe_ir_at_local(counter, 0));
        unsigned one = fe_ir_const(L->m, L->b, FE_IR_I32, 1);
        unsigned next = fe_ir_binary(L->m, L->b, FE_IR_ADD, FE_IR_I32, i, one, 1);
        fe_ir_store(L->m, L->b, fe_ir_at_local(counter, 0), next, FE_IR_I32);
    }
    fe_ir_jmp(L->b, head->id);
    if (L->loop_depth) --L->loop_depth;
    L->b = done;
}

/* `match` over a payload-free enum or an integer: compare the tag against each
   arm's pattern in turn. The checker already proved the arms cover everything,
   so falling off the end cannot happen in a program that compiled -- but the
   generated code has to go somewhere, and going to the join is right. */
static void lower_match(Lower *L, FeNode *n)
{
    FeType *t = n->a ? n->a->sem_type : 0;
    FeIrType it = ir_type(t);
    Slot subject = lower_expr(L, n->a);
    unsigned value;
    FeIrBlock *join;
    FeNode *arm;
    if (it == FE_IR_MEM) { fail(L, "a match over a payload", n); return; }
    value = as_value(L, subject, n->a);
    join = new_block(L);
    for (arm = n->children; arm; arm = arm->next) {
        FeIrBlock *body;
        FeIrBlock *next;
        unsigned want;
        unsigned same;
        FeVariantType *v;
        if (arm->kind != FE_N_ARM) continue;
        if (arm->text && !strcmp(arm->text, "_")) {
            lower_stmt(L, arm->a);
            fe_ir_jmp(L->b, join->id);
            L->b = join;
            return;
        }
        v = t && t->kind == FE_TYPE_ENUM && arm->text
            ? fe_type_variant(t, arm->text) : 0;
        want = fe_ir_const(L->m, L->b, it,
                           v ? (long)v->tag : literal_value(arm));
        same = fe_ir_binary(L->m, L->b, FE_IR_EQ, it, value, want, 1);
        body = new_block(L);
        next = new_block(L);
        fe_ir_br(L->b, same, body->id, next->id);
        L->b = body;
        lower_stmt(L, arm->a);
        fe_ir_jmp(L->b, join->id);
        L->b = next;
    }
    fe_ir_jmp(L->b, join->id);
    L->b = join;
}

static void lower_stmt(Lower *L, FeNode *n)
{
    FeNode *x;
    if (!n || L->failed) return;
    switch (n->kind) {
    case FE_N_BLOCK: {
        unsigned outer = L->owed_count;
        for (x = n->children; x; x = x->next) lower_stmt(L, x);
        /* Leaving a block normally settles what it owes. An exit that jumped
           away already settled on its way out. */
        if (!L->b->terminated) run_deferred(L, outer);
        L->owed_count = outer;
        return;
    }
    case FE_N_LET:
    case FE_N_VAR:
    case FE_N_CONST: {
        unsigned local = declare_var(L, n->cname, n->sem_type, n->text);
        if (n->b) {
            Slot v = lower_expr(L, n->b);
            unsigned flag;
            store_into(L, fe_ir_at_local(local, 0), v, n, ir_size(n->sem_type));
            if (release_flag(L, local, &flag)) {
                unsigned one = fe_ir_const(L->m, L->b, FE_IR_I8, 1);
                fe_ir_store(L->m, L->b, fe_ir_at_local(flag, 0), one, FE_IR_I8);
            }
        }
        return;
    }
    case FE_N_ASSIGN: {
        Slot dst = lower_expr(L, n->a);
        Slot v = lower_expr(L, n->b);
        if (!dst.is_place) { fail(L, "an assignment to a value", n); return; }
        store_into(L, dst.place, v, n, dst.size);
        return;
    }
    case FE_N_EXPR_STMT:
        lower_expr(L, n->a);
        return;
    case FE_N_RETURN:
        lower_return(L, n);
        return;
    case FE_N_IF:
        lower_if(L, n);
        return;
    case FE_N_WHILE:
        lower_while(L, n);
        return;
    case FE_N_BREAK:
        if (L->loop_depth) fe_ir_jmp(L->b, L->break_target[L->loop_depth - 1]);
        return;
    case FE_N_CONTINUE:
        if (L->loop_depth)
            fe_ir_jmp(L->b, L->continue_target[L->loop_depth - 1]);
        return;
    case FE_N_UNSAFE:
        lower_stmt(L, n->a);
        return;
    case FE_N_DEFER:
        if (L->owed_count < 64) {
            L->owed[L->owed_count].block = n->a;
            L->owed[L->owed_count].local = 0;
            L->owed[L->owed_count].flag = 0;
            L->owed[L->owed_count].type = 0;
            ++L->owed_count;
        }
        return;
    case FE_N_FOR:
        lower_for(L, n);
        return;
    case FE_N_MATCH:
        lower_match(L, n);
        return;
    default:
        fail(L, "this statement", n);
        return;
    }
}

/* ------------------------------------------------------------ functions --- */

/* A global is static storage. SPEC 7.1: its initializer is evaluated at
   compile time, so what reaches here is either a constant to place in the
   image or nothing, and the storage starts as zeroes. */
static void lower_global(Lower *L, FeNode *n)
{
    FeType *t = n->sem_type;
    unsigned char *init = 0;
    unsigned long size = ir_size(t);
    if (!n->cname) return;
    if (n->b && n->b->kind == FE_N_LITERAL && size && size <= 8) {
        long v = literal_value(n->b);
        unsigned long i;
        init = (unsigned char *)fe_arena_alloc(&L->m->arena, (size_t)size);
        if (init)
            for (i = 0; i < size; ++i)
                init[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
    }
    fe_ir_global(L->m, n->cname, ir_type(t), size, ir_align(t), init);
}

static int fn_is_generic(const FeNode *fn)
{
    FeNode *p;
    if (!fn) return 0;
    for (p = fn->a ? fn->a->children : 0; p; p = p->next)
        if (p->flags & FE_NODE_COMPTIME) return 1;
    return 0;
}

static void lower_fn_as(Lower *L, FeNode *fn, const char *name)
{
    FeNode *p;
    FeType *ret = fn->b ? fe_type_from_ast(&L->c->types, fn->b) : 0;
    FeIrFunc *f;
    if (!name) return;
    f = fe_ir_func(L->m, name, ir_type(ret), ir_size(ret));
    if (!f) return;
    L->fn = f;
    L->ret_type = ret;
    L->var_count = 0;
    L->loop_depth = 0;
    /* A hidden first parameter holds where an aggregate result goes. */
    if (f->returns_by_address)
        L->ret_local = fe_ir_local(L->m, f, FE_IR_PTR, 4, 4, "result");
    for (p = fn->a ? fn->a->children : 0; p; p = p->next) {
        FeType *pt;
        int by_address;
        unsigned local;
        /* A comptime parameter was consumed at compile time; it has no
           storage and takes no argument slot. */
        if (p->flags & FE_NODE_COMPTIME) continue;
        pt = fe_type_from_ast(&L->c->types, p->a);
        /* An aggregate parameter arrives as an address. */
        by_address = ir_type(pt) == FE_IR_MEM;
        local = by_address
            ? fe_ir_local(L->m, f, FE_IR_PTR, 4, 4, p->text)
            : fe_ir_local(L->m, f, ir_type(pt), ir_size(pt), ir_align(pt),
                          p->text);
        if (L->var_count < LOWER_MAX_LOCALS) {
            L->vars[L->var_count].cname = p->cname;
            L->vars[L->var_count].local = local;
            L->vars[L->var_count].by_address = by_address;
            ++L->var_count;
        }
    }
    f->param_count = f->local_count;
    L->b = fe_ir_block(L->m, f);
    lower_stmt(L, fn->c);
    /* A void function may just run off the end. */
    fe_ir_ret(L->b, 0, 0);
}

static void lower_fn(Lower *L, FeNode *fn)
{
    lower_fn_as(L, fn, fn->cname);
}

int fe_lower_program(FeCheck *c, FeIrModule *out)
{
    Lower L;
    unsigned u;
    FeNode *n;
    memset(&L, 0, sizeof L);
    L.c = c;
    L.m = out;
    /* The codes have to be known while the bodies are lowered, so the names
       are gathered from the whole build first. */
    for (u = 0; u < c->build->count; ++u)
        collect_error_names(&L, c->build->units[u].ast.root);
    for (u = 0; u < c->build->count; ++u) {
        FeUnit *unit = &c->build->units[u];
        c->ast = &unit->ast;
        c->unit = unit;
        c->types.unit_name = unit->name[0] ? unit->name : "unit";
        if (!out->unit_file || !out->unit_file[0]) out->unit_file = unit->path;
        for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next)
            if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST)
                lower_global(&L, n);
            else if (n->kind == FE_N_FN && !n->c) {
                /* A declaration with no body is something the linker will
                   find: the runtime, or a C library. */
                FeType *ret = n->b ? fe_type_from_ast(&c->types, n->b) : 0;
                FeIrFunc *f;
                if (!n->cname) continue;
                f = fe_ir_func(out, n->cname, ir_type(ret), ir_size(ret));
                if (f) f->is_extern = 1;
            }
            else if (n->kind == FE_N_FN && n->c && !fn_is_generic(n)) {
                lower_fn(&L, n);
                /* The entry unit is the one the build was rooted at. */
                if (u == 0 && n->text && !strcmp(n->text, "main"))
                    out->entry_main = n->cname;
            }
    }
    /* Each instance the checker reached is a function of its own: the same
       body, read with different types bound, under its own link name. This is
       where monomorphisation actually produces code -- the front end only
       decided which instances exist. */
    for (u = 0; u < c->instance_count && !L.failed; ++u) {
        FeInstance *inst = &c->instances[u];
        FeUnit *home;
        FeTypeBind save[FE_TYPE_PARAM_MAX];
        unsigned save_count;
        unsigned k;
        if (!inst->decl || !inst->decl->c || !inst->cname || !inst->home)
            continue;
        home = 0;
        for (k = 0; k < c->build->count; ++k)
            if (!strcmp(c->build->units[k].name, inst->home))
                home = &c->build->units[k];
        if (!home) continue;
        c->ast = &home->ast;
        c->unit = home;
        c->types.unit_name = home->name;
        save_count = c->types.param_count;
        for (k = 0; k < FE_TYPE_PARAM_MAX; ++k) save[k] = c->types.params[k];
        c->types.param_count = inst->bind_count;
        for (k = 0; k < inst->bind_count && k < FE_TYPE_PARAM_MAX; ++k)
            c->types.params[k] = inst->binds[k];
        lower_fn_as(&L, inst->decl, inst->cname);
        c->types.param_count = save_count;
        for (k = 0; k < FE_TYPE_PARAM_MAX; ++k) c->types.params[k] = save[k];
    }
    return !L.failed;
}
