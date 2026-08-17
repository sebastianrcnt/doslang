#include "lowerpri.h"

void fail(Lower *L, const char *why, FeNode *n)
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
FeIrType ir_type_of(const FeType *t)
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
    case FE_TYPE_REF:
    case FE_TYPE_RAW:   return FE_IR_PTR;
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

int enum_has_payload(const FeType *t)
{
    unsigned i;
    if (!t || t->kind != FE_TYPE_ENUM) return 0;
    for (i = 0; i < t->variant_count; ++i)
        if (t->variants[i].field_count) return 1;
    return 0;
}

FeIrType ir_type(const FeType *t)
{
    if (t && t->kind == FE_TYPE_ENUM && enum_has_payload(t)) return FE_IR_MEM;
    return ir_type_of(t);
}

unsigned long ir_size(const FeType *t)
{
    return t ? fe_type_size(t) : 0UL;
}

unsigned ir_align(const FeType *t)
{
    return t ? fe_type_align(t) : 1U;
}

int type_is_unsigned(const FeType *t)
{
    return t && t->kind == FE_TYPE_INT && t->is_unsigned;
}

/* ---------------------------------------------------------------- slots --- */

Slot slot_value(unsigned temp, FeIrType t)
{
    Slot s;
    s.is_place = 0; s.temp = temp; s.type = t; s.size = 0;
    s.place = fe_ir_at_temp(0, 0);
    return s;
}

Slot slot_place(FeIrPlace p, FeIrType t, unsigned long size)
{
    Slot s;
    s.is_place = 1; s.temp = 0; s.place = p; s.type = t; s.size = size;
    return s;
}

Slot slot_void(void)
{
    return slot_value(0, FE_IR_VOID);
}

/* Read a slot as a value. An aggregate has no value form, so asking for one is
   a lowering bug rather than a program error. */
unsigned as_value(Lower *L, Slot s, FeNode *n)
{
    if (!s.is_place) return s.temp;
    if (s.type == FE_IR_MEM) { fail(L, "an aggregate as a value", n); return 0; }
    return fe_ir_load(L->m, L->b, s.type, s.place);
}

/* The address of a slot. */
unsigned as_address(Lower *L, Slot s, FeNode *n)
{
    if (!s.is_place) { fail(L, "the address of a temporary", n); return 0; }
    return fe_ir_addr(L->m, L->b, s.place);
}

/* --------------------------------------------------------------- locals --- */

/* Does letting go of this type have to do something? */
int needs_release(const FeType *t)
{
    unsigned i;
    if (!t) return 0;
    if (t->kind == FE_TYPE_OWNED) return 1;
    if (t->has_drop) return 1;
    /* SPEC 5 R1: letting go of an owner lets go of what it owns. A struct that
       holds an owner has something to do even when it says nothing itself --
       which is what lets one type hold another that has a `drop`, since
       calling `drop` by hand is not allowed. */
    if (t->kind == FE_TYPE_STRUCT)
        for (i = 0; i < t->field_count; ++i)
            if (needs_release(t->fields[i].type)) return 1;
    return 0;
}

int lower_reserve(Lower *L, void **items, unsigned *capacity, unsigned needed,
                  unsigned long item_size)
{
    unsigned want;
    void *grown;
    if (needed < *capacity) return 1;
    want = *capacity ? *capacity * 2U : 16U;
    while (want <= needed) want *= 2U;
    grown = fe_arena_alloc(&L->m->arena, (size_t)(want * item_size));
    if (!grown) { fail(L, "a function this large", 0); return 0; }
    if (*items) memcpy(grown, *items, (size_t)(*capacity * item_size));
    *items = grown;
    *capacity = want;
    return 1;
}

unsigned declare_var(Lower *L, const char *cname, const FeType *t,
                            const char *name)
{
    unsigned local = fe_ir_local(L->m, L->fn, ir_type(t), ir_size(t),
                                 ir_align(t), name);
    if (lower_reserve(L, (void **)&L->vars, &L->var_capacity, L->var_count,
                      (unsigned long)sizeof(LowerVar))) {
        L->vars[L->var_count].cname = cname;
        L->vars[L->var_count].local = local;
        L->vars[L->var_count].by_address = 0;
        ++L->var_count;
    }
    if (needs_release(t) &&
        lower_reserve(L, (void **)&L->owed, &L->owed_capacity, L->owed_count,
                      (unsigned long)sizeof *L->owed)) {
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
int release_flag(Lower *L, unsigned local, unsigned *flag)
{
    unsigned i;
    for (i = L->owed_count; i > 0; --i)
        if (!L->owed[i - 1].block && L->owed[i - 1].local == local) {
            *flag = L->owed[i - 1].flag;
            return 1;
        }
    return 0;
}

LowerVar *find_var(Lower *L, const char *cname)
{
    unsigned i;
    if (!cname) return 0;
    for (i = L->var_count; i > 0; --i)
        if (L->vars[i - 1].cname && strcmp(L->vars[i - 1].cname, cname) == 0)
            return &L->vars[i - 1];
    return 0;
}

/* --------------------------------------------------------------- blocks --- */

FeIrBlock *new_block(Lower *L)
{
    return fe_ir_block(L->m, L->fn);
}

/* Which file a trap raised right now came from. The whole build lowers into
   one module, so the unit being lowered is the only thing that knows. */
unsigned trap_file(Lower *L)
{
    return fe_ir_file(L->m, L->c->unit ? L->c->unit->path : "");
}

/* A check that must hold. `ok` is a condition; when it is false the program
   stops where it is. `--no-checks` removes the comparison and the branch, not
   just the message, which is the whole point of the flag. */
void guard(Lower *L, unsigned ok, FeIrTrap reason, unsigned long line)
{
    FeIrBlock *bad = new_block(L);
    FeIrBlock *cont = new_block(L);
    fe_ir_br(L->b, ok, cont->id, bad->id);
    L->b = bad;
    fe_ir_trap(L->b, reason, line, trap_file(L));
    L->b = cont;
}

/* A tag says which of the two things a wrapper holds. An optional is one byte
   at the front unless the payload has a spare representation; an error union is
   a two-byte error code, and zero means there is no error. */
FeIrType tag_type(const FeType *t)
{
    return t && t->kind == FE_TYPE_ERROR_UNION ? FE_IR_I16 : FE_IR_I8;
}

int uses_niche(const FeType *t)
{
    return t && t->kind == FE_TYPE_OPTIONAL && fe_m7_optional_uses_niche(t->elem);
}

/* Somewhere to build an aggregate that has no home of its own yet. */
unsigned scratch(Lower *L, const FeType *t, const char *why)
{
    return fe_ir_local(L->m, L->fn, ir_type(t), ir_size(t), ir_align(t), why);
}


/* The number of elements an indexable place holds, and where the first element
   is. An array is its own storage; a slice points at someone else's. */
void indexable_parts(Lower *L, Slot base, const FeType *t,
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

void note_error_name(Lower *L, const char *name)
{
    unsigned i;
    unsigned at;
    if (!name) return;
    if (!lower_reserve(L, (void **)&L->error_names, &L->error_capacity,
                       L->error_count, (unsigned long)sizeof(const char *)))
        return;
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

void collect_error_names(Lower *L, FeNode *n)
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

long error_code(Lower *L, const char *name)
{
    unsigned i;
    for (i = 0; i < L->error_count; ++i)
        if (!strcmp(L->error_names[i], name)) return (long)(i + 1);
    return 0;
}

/* ---------------------------------------------------------- expressions --- */

FeIrOp binary_op(const char *op, int *is_cmp)
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

long literal_value(FeNode *n)
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
    {
        /* SPEC 3 spells four radices. Reading `0b1010` as decimal stops at the
           `b` and answers zero, which is a number and so goes unnoticed. */
        int base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) { base = 2; s += 2; }
        else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) { base = 8; s += 2; }
        for (; *s; ++s) {
            int d;
            if (*s == '_') continue;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
            else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
            else break;
            if (d >= base) break;
            v = v * base + d;
        }
    }
    return neg ? -v : v;
}

/* `and` and `or` do not evaluate the right side unless they have to, so they
   are control flow rather than an operation. */
Slot lower_logical(Lower *L, FeNode *n, int is_and)
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
int lower_builtin(Lower *L, FeNode *n, Slot *out)
{
    const char *name = n->text;
    if (!name || name[0] != '@') return 0;
    if (!strcmp(name, "@trap")) {
        fe_ir_trap(L->b, FE_TRAP_EXPLICIT, n->loc.line, trap_file(L));
        L->b = new_block(L);
        *out = slot_void();
        return 1;
    }
    if (!strcmp(name, "@unreachable")) {
        fe_ir_trap(L->b, FE_TRAP_UNREACHABLE, n->loc.line, trap_file(L));
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
    if (!strcmp(name, "@volatile_load")) {
        /* Reading through a raw pointer. Nothing here reorders loads yet, so
           volatile and ordinary read the same; the keyword is what marks the
           access as deliberate, and the checker already required `unsafe`. */
        FeNode *arg = n->children;
        unsigned p = as_value(L, lower_expr(L, arg), arg);
        FeIrType t = ir_type(n->sem_type);
        if (t == FE_IR_VOID || t == FE_IR_MEM) t = FE_IR_I8;
        *out = slot_place(fe_ir_at_temp(p, 0), t, ir_size(n->sem_type));
        return 1;
    }
    if (!strcmp(name, "@volatile_store")) {
        FeNode *arg = n->children;
        FeNode *value = arg ? arg->next : 0;
        unsigned p = as_value(L, lower_expr(L, arg), arg);
        Slot v = lower_expr(L, value);
        FeIrType t = value && value->sem_type ? ir_type(value->sem_type)
                                              : FE_IR_I8;
        fe_ir_store(L->m, L->b, fe_ir_at_temp(p, 0), as_value(L, v, value), t);
        *out = slot_void();
        return 1;
    }
    if (!strcmp(name, "@ptr_cast")) {
        /* A pointer is a pointer; the type it is said to point at is the
           checker's business and leaves no trace here. */
        FeNode *arg = n->children;
        FeNode *value = arg ? arg->next : 0;
        *out = slot_value(as_value(L, lower_expr(L, value), value), FE_IR_PTR);
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

int is_mem_call(const FeNode *n, const char *what)
{
    return n && n->a && n->a->kind == FE_N_MEMBER &&
           n->a->a && n->a->a->kind == FE_N_IDENT && n->a->a->text &&
           !strcmp(n->a->a->text, "mem") &&
           n->a->b && n->a->b->text && !strcmp(n->a->b->text, what);
}

/* Build `!^T`: zero and the pointer when the allocation worked, the
   out-of-memory code when it did not. */
Slot allocation_result(Lower *L, FeNode *n, unsigned pointer)
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

int lower_mem(Lower *L, FeNode *n, Slot *out)
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

/* ------------------------------------------------------------ printing --- *
 * SPEC 6.3.1: the formatting builtins are not variadic functions. A call is
 * expanded here into one write per literal chunk and one per value, so the
 * language never grows a variadic calling convention and the format string is
 * gone by the time anything runs.
 * -------------------------------------------------------------------------- */

/* Write `len` bytes of a literal that is already in the image. */
