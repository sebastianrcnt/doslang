#include "lower.h"
#include <string.h>
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

static unsigned declare_var(Lower *L, const char *cname, const FeType *t,
                            const char *name)
{
    unsigned local = fe_ir_local(L->m, L->fn, ir_type(t), ir_size(t),
                                 ir_align(t), name);
    if (L->var_count < LOWER_MAX_LOCALS) {
        L->vars[L->var_count].cname = cname;
        L->vars[L->var_count].local = local;
        ++L->var_count;
    }
    return local;
}

static int find_var(Lower *L, const char *cname, unsigned *out)
{
    unsigned i;
    if (!cname) return 0;
    for (i = L->var_count; i > 0; --i)
        if (L->vars[i - 1].cname && strcmp(L->vars[i - 1].cname, cname) == 0) {
            *out = L->vars[i - 1].local;
            return 1;
        }
    return 0;
}

/* --------------------------------------------------------------- blocks --- */

static FeIrBlock *new_block(Lower *L)
{
    return fe_ir_block(L->m, L->fn);
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
    fe_ir_store(L->m, L->b, fe_ir_at_local(result, 0), left);
    if (is_and) fe_ir_br(L->b, left, rhs->id, join->id);
    else fe_ir_br(L->b, left, join->id, rhs->id);
    L->b = rhs;
    right = as_value(L, lower_expr(L, n->b), n->b);
    fe_ir_store(L->m, L->b, fe_ir_at_local(result, 0), right);
    fe_ir_jmp(L->b, join->id);
    L->b = join;
    return slot_place(fe_ir_at_local(result, 0), FE_IR_I8, 1);
}

static Slot lower_call(Lower *L, FeNode *n)
{
    unsigned args[16];
    unsigned count = 0;
    FeNode *arg;
    FeType *ret = n->sem_type;
    FeIrType rt = ir_type(ret);
    unsigned result_local = 0;
    const char *callee = n->a && n->a->cname ? n->a->cname :
                         (n->sem_decl && n->sem_decl->cname ?
                          n->sem_decl->cname : 0);
    if (!callee) { fail(L, "a call with no target", n); return slot_void(); }
    /* An aggregate result is written through a hidden first argument. */
    if (rt == FE_IR_MEM) {
        result_local = fe_ir_local(L->m, L->fn, FE_IR_MEM, ir_size(ret),
                                   ir_align(ret), "result");
        args[count++] = fe_ir_addr(L->m, L->b, fe_ir_at_local(result_local, 0));
    }
    for (arg = n->children; arg; arg = arg->next) {
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

static Slot lower_expr(Lower *L, FeNode *n)
{
    FeType *t;
    FeIrType it;
    if (!n || L->failed) return slot_void();
    t = n->sem_type;
    it = ir_type(t);
    switch (n->kind) {
    case FE_N_LITERAL:
        return slot_value(fe_ir_const(L->m, L->b,
                                      it == FE_IR_VOID ? FE_IR_I32 : it,
                                      literal_value(n)),
                          it == FE_IR_VOID ? FE_IR_I32 : it);
    case FE_N_IDENT: {
        unsigned local;
        if (find_var(L, n->cname, &local))
            return slot_place(fe_ir_at_local(local, 0), it, ir_size(t));
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
        /* `p.^` reads through a pointer. */
        if (n->text && !strcmp(n->text, ".^")) {
            unsigned p = as_value(L, lower_expr(L, n->a), n->a);
            return slot_place(fe_ir_at_temp(p, 0), it, ir_size(t));
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
    case FE_N_CALL:
        return lower_call(L, n);
    case FE_N_EXPR:
        return lower_expr(L, n->a);
    default:
        fail(L, "this expression", n);
        return slot_void();
    }
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
    fe_ir_store(L->m, L->b, dst, as_value(L, value, n));
}

static void lower_return(Lower *L, FeNode *n)
{
    Slot v;
    if (!n->a) { fe_ir_ret(L->b, 0, 0); return; }
    v = lower_expr(L, n->a);
    if (L->fn->returns_by_address) {
        store_into(L, fe_ir_at_temp(L->ret_local, 0), v, n,
                   ir_size(L->ret_type));
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

static void lower_stmt(Lower *L, FeNode *n)
{
    FeNode *x;
    if (!n || L->failed) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        for (x = n->children; x; x = x->next) lower_stmt(L, x);
        return;
    case FE_N_LET:
    case FE_N_VAR:
    case FE_N_CONST: {
        unsigned local = declare_var(L, n->cname, n->sem_type, n->text);
        if (n->b) {
            Slot v = lower_expr(L, n->b);
            store_into(L, fe_ir_at_local(local, 0), v, n, ir_size(n->sem_type));
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
    default:
        fail(L, "this statement", n);
        return;
    }
}

/* ------------------------------------------------------------ functions --- */

static void lower_fn(Lower *L, FeNode *fn)
{
    FeNode *p;
    FeType *ret = fn->b ? fe_type_from_ast(&L->c->types, fn->b) : 0;
    FeIrFunc *f;
    if (!fn->cname) return;
    f = fe_ir_func(L->m, fn->cname, ir_type(ret), ir_size(ret));
    if (!f) return;
    L->fn = f;
    L->ret_type = ret;
    L->var_count = 0;
    L->loop_depth = 0;
    /* A hidden first parameter holds where an aggregate result goes. */
    if (f->returns_by_address)
        L->ret_local = fe_ir_local(L->m, f, FE_IR_PTR, 4, 4, "result");
    for (p = fn->a ? fn->a->children : 0; p; p = p->next) {
        FeType *pt = fe_type_from_ast(&L->c->types, p->a);
        /* An aggregate parameter arrives as an address. */
        unsigned local = ir_type(pt) == FE_IR_MEM
            ? fe_ir_local(L->m, f, FE_IR_PTR, 4, 4, p->text)
            : fe_ir_local(L->m, f, ir_type(pt), ir_size(pt), ir_align(pt),
                          p->text);
        if (L->var_count < LOWER_MAX_LOCALS) {
            L->vars[L->var_count].cname = p->cname;
            L->vars[L->var_count].local = local;
            ++L->var_count;
        }
    }
    f->param_count = f->local_count;
    L->b = fe_ir_block(L->m, f);
    lower_stmt(L, fn->c);
    /* A void function may just run off the end. */
    fe_ir_ret(L->b, 0, 0);
}

int fe_lower_program(FeCheck *c, FeIrModule *out)
{
    Lower L;
    unsigned u;
    FeNode *n;
    memset(&L, 0, sizeof L);
    L.c = c;
    L.m = out;
    for (u = 0; u < c->build->count; ++u) {
        FeUnit *unit = &c->build->units[u];
        c->ast = &unit->ast;
        c->unit = unit;
        c->types.unit_name = unit->name[0] ? unit->name : "unit";
        if (!out->unit_file || !out->unit_file[0]) out->unit_file = unit->path;
        for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next)
            if (n->kind == FE_N_FN && n->c) lower_fn(&L, n);
    }
    return !L.failed;
}
