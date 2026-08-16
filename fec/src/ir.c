#include "ir.h"
#include <string.h>
#include <stdio.h>

void fe_ir_module_init(FeIrModule *m)
{
    fe_arena_init(&m->arena, 16384);
    m->unit_file = "";
    m->entry_main = 0;
    m->funcs = 0;
    m->last_func = 0;
    m->globals = 0;
    m->last_global = 0;
}

void fe_ir_module_destroy(FeIrModule *m)
{
    fe_arena_destroy(&m->arena);
    m->funcs = 0;
    m->last_func = 0;
    m->globals = 0;
    m->last_global = 0;
}

static void *ir_alloc(FeIrModule *m, unsigned long size)
{
    return fe_arena_alloc(&m->arena, (size_t)size);
}

FeIrFunc *fe_ir_func(FeIrModule *m, const char *name, FeIrType ret,
                     unsigned long ret_size)
{
    FeIrFunc *f = (FeIrFunc *)ir_alloc(m, sizeof(FeIrFunc));
    if (!f) return 0;
    memset(f, 0, sizeof *f);
    f->name = name;
    f->ret = ret;
    f->ret_size = ret_size;
    /* An aggregate result is written through a hidden first parameter, so the
       caller owns the storage and no size threshold has to be agreed on. */
    f->returns_by_address = ret == FE_IR_MEM;
    if (m->last_func) m->last_func->next = f;
    else m->funcs = f;
    m->last_func = f;
    return f;
}

unsigned fe_ir_local(FeIrModule *m, FeIrFunc *f, FeIrType type,
                     unsigned long size, unsigned align, const char *name)
{
    if (f->local_count == f->local_capacity) {
        unsigned cap = f->local_capacity ? f->local_capacity * 2U : 8U;
        FeIrLocal *grown = (FeIrLocal *)ir_alloc(m, cap * sizeof(FeIrLocal));
        if (!grown) return 0;
        if (f->locals)
            memcpy(grown, f->locals, f->local_count * sizeof(FeIrLocal));
        f->locals = grown;
        f->local_capacity = cap;
    }
    f->locals[f->local_count].type = type;
    f->locals[f->local_count].size = size;
    f->locals[f->local_count].align = align ? align : 1U;
    f->locals[f->local_count].name = name;
    return f->local_count++;
}

unsigned fe_ir_temp(FeIrFunc *f)
{
    return f->temp_count++;
}

FeIrBlock *fe_ir_block(FeIrModule *m, FeIrFunc *f)
{
    FeIrBlock *b = (FeIrBlock *)ir_alloc(m, sizeof(FeIrBlock));
    if (!b) return 0;
    memset(b, 0, sizeof *b);
    b->id = f->block_count++;
    b->func = f;
    /* Until something says otherwise a block falls off the end, which is only
       correct for a void function; lowering always sets a real terminator. */
    b->term = FE_IR_RET;
    if (f->last) f->last->next = b;
    else f->first = b;
    f->last = b;
    return b;
}

FeIrGlobal *fe_ir_global(FeIrModule *m, const char *name, FeIrType type,
                         unsigned long size, unsigned align,
                         const unsigned char *init)
{
    FeIrGlobal *g;
    for (g = m->globals; g; g = g->next)
        if (!strcmp(g->name, name)) return g;
    g = (FeIrGlobal *)ir_alloc(m, sizeof(FeIrGlobal));
    if (!g) return 0;
    memset(g, 0, sizeof *g);
    g->name = name;
    g->type = type;
    g->size = size;
    g->align = align ? align : 1U;
    g->init = init;
    if (m->last_global) m->last_global->next = g;
    else m->globals = g;
    m->last_global = g;
    return g;
}

void fe_ir_global_ref(FeIrModule *m, FeIrGlobal *g, unsigned long at,
                      const char *symbol)
{
    FeIrReloc *grown;
    if (!g) return;
    grown = (FeIrReloc *)ir_alloc(m, (g->reloc_count + 1) * sizeof(FeIrReloc));
    if (!grown) return;
    if (g->relocs) memcpy(grown, g->relocs, g->reloc_count * sizeof(FeIrReloc));
    grown[g->reloc_count].at = at;
    grown[g->reloc_count].symbol = symbol;
    g->relocs = grown;
    ++g->reloc_count;
}

const char *fe_ir_string(FeIrModule *m, const char *bytes, unsigned long length)
{
    FeIrGlobal *g;
    unsigned char *copy;
    char *name;
    unsigned serial = 0;
    /* The same text twice is the same storage: string literals are read-only,
       so sharing them is free. */
    for (g = m->globals; g; g = g->next) {
        if (g->init && g->size == length &&
            !memcmp(g->init, bytes, (size_t)length)) return g->name;
        ++serial;
    }
    copy = (unsigned char *)ir_alloc(m, length ? length : 1UL);
    if (!copy) return 0;
    if (length) memcpy(copy, bytes, (size_t)length);
    name = (char *)ir_alloc(m, 32);
    if (!name) return 0;
    sprintf(name, "FE_STR_%u", serial);
    g = fe_ir_global(m, name, FE_IR_MEM, length, 1, copy);
    return g ? g->name : 0;
}

FeIrPlace fe_ir_at_local(unsigned index, long offset)
{
    FeIrPlace p;
    p.base = FE_PLACE_LOCAL; p.index = index; p.name = 0; p.offset = offset;
    return p;
}

FeIrPlace fe_ir_at_global(const char *name, long offset)
{
    FeIrPlace p;
    p.base = FE_PLACE_GLOBAL; p.index = 0; p.name = name; p.offset = offset;
    return p;
}

FeIrPlace fe_ir_at_temp(unsigned temp, long offset)
{
    FeIrPlace p;
    p.base = FE_PLACE_TEMP; p.index = temp; p.name = 0; p.offset = offset;
    return p;
}

static FeIrValue *emit(FeIrModule *m, FeIrBlock *b, FeIrOp op, FeIrType t)
{
    FeIrValue *v = (FeIrValue *)ir_alloc(m, sizeof(FeIrValue));
    if (!v) return 0;
    memset(v, 0, sizeof *v);
    v->op = op;
    v->type = t;
    if (b->last) b->last->next = v;
    else b->first = v;
    b->last = v;
    return v;
}

/* A result needs a fresh temporary, and the counter lives on the function, so
   a block carries the function it is being built in. */
static unsigned result(FeIrModule *m, FeIrBlock *b, FeIrValue *v)
{
    (void)m;
    v->has_dest = 1;
    v->dest = fe_ir_temp(b->func);
    return v->dest;
}

unsigned fe_ir_const(FeIrModule *m, FeIrBlock *b, FeIrType t, long value)
{
    FeIrValue *v = emit(m, b, FE_IR_CONST, t);
    if (!v) return 0;
    v->imm = value;
    return result(m, b, v);
}

unsigned fe_ir_load(FeIrModule *m, FeIrBlock *b, FeIrType t, FeIrPlace p)
{
    FeIrValue *v = emit(m, b, FE_IR_LOAD, t);
    if (!v) return 0;
    v->place = p;
    return result(m, b, v);
}

void fe_ir_store(FeIrModule *m, FeIrBlock *b, FeIrPlace p, unsigned value,
                 FeIrType t)
{
    FeIrValue *v = emit(m, b, FE_IR_STORE, t);
    if (!v) return;
    v->place = p;
    v->a = value;
}

unsigned fe_ir_addr(FeIrModule *m, FeIrBlock *b, FeIrPlace p)
{
    FeIrValue *v = emit(m, b, FE_IR_ADDR, FE_IR_PTR);
    if (!v) return 0;
    v->place = p;
    return result(m, b, v);
}

unsigned fe_ir_binary(FeIrModule *m, FeIrBlock *b, FeIrOp op, FeIrType t,
                      unsigned a, unsigned c, int is_unsigned)
{
    FeIrValue *v;
    int is_cmp = op >= FE_IR_EQ && op <= FE_IR_GE;
    v = emit(m, b, op, is_cmp ? FE_IR_I8 : t);
    if (!v) return 0;
    v->a = a;
    v->b = c;
    v->is_unsigned = is_unsigned;
    /* A comparison reports i8 but reads its operands at `t`, so the width has
       to survive somewhere the backend can see it. */
    if (is_cmp) v->imm = (long)t;
    return result(m, b, v);
}

unsigned fe_ir_cast(FeIrModule *m, FeIrBlock *b, FeIrType from, FeIrType to,
                    unsigned a, int is_unsigned)
{
    FeIrValue *v = emit(m, b, FE_IR_CAST, to);
    if (!v) return 0;
    v->a = a;
    v->imm = (long)from;
    v->is_unsigned = is_unsigned;
    return result(m, b, v);
}

unsigned fe_ir_call(FeIrModule *m, FeIrBlock *b, FeIrType ret,
                    const char *callee, unsigned *args, unsigned count)
{
    FeIrValue *v = emit(m, b, FE_IR_CALL, ret);
    unsigned i;
    if (!v) return 0;
    v->callee = callee;
    v->arg_count = count;
    if (count) {
        v->args = (unsigned *)ir_alloc(m, count * sizeof(unsigned));
        if (v->args) for (i = 0; i < count; ++i) v->args[i] = args[i];
        else v->arg_count = 0;
    }
    if (ret == FE_IR_VOID) return 0;
    return result(m, b, v);
}

void fe_ir_copy(FeIrModule *m, FeIrBlock *b, FeIrPlace dst, FeIrPlace src,
                unsigned long size)
{
    FeIrValue *v = emit(m, b, FE_IR_COPY, FE_IR_VOID);
    if (!v) return;
    v->place = dst;
    v->place2 = src;
    v->imm = (long)size;
}

void fe_ir_jmp(FeIrBlock *b, unsigned target)
{
    if (b->terminated) return;
    b->terminated = 1;
    b->term = FE_IR_JMP;
    b->target = target;
}

void fe_ir_br(FeIrBlock *b, unsigned cond, unsigned t, unsigned f)
{
    if (b->terminated) return;
    b->terminated = 1;
    b->term = FE_IR_BR;
    b->cond = cond;
    b->target = t;
    b->target_else = f;
}

void fe_ir_ret(FeIrBlock *b, unsigned value, int has_value)
{
    if (b->terminated) return;
    b->terminated = 1;
    b->term = FE_IR_RET;
    b->ret_value = value;
    b->has_ret_value = has_value;
}

void fe_ir_trap(FeIrBlock *b, FeIrTrap reason, unsigned long line)
{
    if (b->terminated) return;
    b->terminated = 1;
    b->term = FE_IR_TRAP;
    b->trap = reason;
    b->trap_line = line;
}

const char *fe_ir_type_name(FeIrType t)
{
    switch (t) {
    case FE_IR_VOID: return "void";
    case FE_IR_I8:   return "i8";
    case FE_IR_I16:  return "i16";
    case FE_IR_I32:  return "i32";
    case FE_IR_PTR:  return "ptr";
    case FE_IR_MEM:  return "mem";
    }
    return "?";
}

const char *fe_ir_op_name(FeIrOp op)
{
    switch (op) {
    case FE_IR_CONST: return "const";
    case FE_IR_LOAD:  return "load";
    case FE_IR_STORE: return "store";
    case FE_IR_ADDR:  return "addr";
    case FE_IR_ADD:   return "add";
    case FE_IR_SUB:   return "sub";
    case FE_IR_MUL:   return "mul";
    case FE_IR_DIV:   return "div";
    case FE_IR_MOD:   return "mod";
    case FE_IR_AND:   return "and";
    case FE_IR_OR:    return "or";
    case FE_IR_XOR:   return "xor";
    case FE_IR_SHL:   return "shl";
    case FE_IR_SHR:   return "shr";
    case FE_IR_EQ:    return "eq";
    case FE_IR_NE:    return "ne";
    case FE_IR_LT:    return "lt";
    case FE_IR_LE:    return "le";
    case FE_IR_GT:    return "gt";
    case FE_IR_GE:    return "ge";
    case FE_IR_CAST:  return "cast";
    case FE_IR_CALL:  return "call";
    case FE_IR_COPY:  return "copy";
    }
    return "?";
}

static const char *trap_name(FeIrTrap t)
{
    switch (t) {
    case FE_TRAP_BOUNDS:      return "bounds";
    case FE_TRAP_OVERFLOW:    return "overflow";
    case FE_TRAP_DIVIDE:      return "divide";
    case FE_TRAP_UNREACHABLE: return "unreachable";
    case FE_TRAP_EXPLICIT:    return "trap";
    }
    return "?";
}

static void dump_place(const FeIrPlace *p, FILE *out)
{
    switch (p->base) {
    case FE_PLACE_LOCAL:  fprintf(out, "$%u", p->index); break;
    case FE_PLACE_GLOBAL: fprintf(out, "@%s", p->name ? p->name : "?"); break;
    case FE_PLACE_TEMP:   fprintf(out, "%%%u", p->index); break;
    }
    if (p->offset) fprintf(out, " + %ld", p->offset);
}

static void dump_value(const FeIrValue *v, FILE *out)
{
    unsigned i;
    fputs("    ", out);
    if (v->has_dest) fprintf(out, "%%%u = ", v->dest);
    switch (v->op) {
    case FE_IR_CONST:
        fprintf(out, "const %s %ld", fe_ir_type_name(v->type), v->imm);
        break;
    case FE_IR_LOAD:
        fprintf(out, "load %s ", fe_ir_type_name(v->type));
        dump_place(&v->place, out);
        break;
    case FE_IR_STORE:
        fputs("store ", out);
        dump_place(&v->place, out);
        fprintf(out, ", %%%u", v->a);
        break;
    case FE_IR_ADDR:
        fputs("addr ", out);
        dump_place(&v->place, out);
        break;
    case FE_IR_CAST:
        fprintf(out, "cast %s %s %%%u",
                fe_ir_type_name((FeIrType)v->imm),
                fe_ir_type_name(v->type), v->a);
        break;
    case FE_IR_CALL:
        fprintf(out, "call @%s(", v->callee ? v->callee : "?");
        for (i = 0; i < v->arg_count; ++i)
            fprintf(out, "%s%%%u", i ? ", " : "", v->args[i]);
        fputc(')', out);
        break;
    case FE_IR_COPY:
        fputs("copy ", out);
        dump_place(&v->place, out);
        fputs(", ", out);
        dump_place(&v->place2, out);
        fprintf(out, ", %ld", v->imm);
        break;
    default:
        fprintf(out, "%s %s %%%u, %%%u", fe_ir_op_name(v->op),
                fe_ir_type_name(v->op >= FE_IR_EQ && v->op <= FE_IR_GE ?
                                (FeIrType)v->imm : v->type), v->a, v->b);
        if (v->is_unsigned) fputs(" u", out);
        break;
    }
    fputc('\n', out);
}

void fe_ir_dump(const FeIrModule *m, FILE *out)
{
    const FeIrFunc *f;
    const FeIrBlock *b;
    const FeIrValue *v;
    const FeIrGlobal *g;
    unsigned i;
    if (m->unit_file && m->unit_file[0])
        fprintf(out, "; unit file %s\n", m->unit_file);
    for (g = m->globals; g; g = g->next)
        fprintf(out, "global @%s : %s %lu\n", g->name,
                fe_ir_type_name(g->type), g->size);
    for (f = m->funcs; f; f = f->next) {
        if (f->is_extern) {
            fprintf(out, "extern fn @%s -> %s\n", f->name,
                    fe_ir_type_name(f->ret));
            continue;
        }
        fprintf(out, "fn @%s -> %s%s {\n", f->name, fe_ir_type_name(f->ret),
                f->returns_by_address ? " (by address)" : "");
        for (i = 0; i < f->local_count; ++i) {
            fprintf(out, "  $%u: %s", i, fe_ir_type_name(f->locals[i].type));
            if (f->locals[i].type == FE_IR_MEM)
                fprintf(out, "<%lu>", f->locals[i].size);
            if (i < f->param_count) fputs("  ; parameter", out);
            if (f->locals[i].name) fprintf(out, "  ; %s", f->locals[i].name);
            fputc('\n', out);
        }
        for (b = f->first; b; b = b->next) {
            fprintf(out, "  b%u:\n", b->id);
            for (v = b->first; v; v = v->next) dump_value(v, out);
            switch (b->term) {
            case FE_IR_JMP:
                fprintf(out, "    jmp b%u\n", b->target); break;
            case FE_IR_BR:
                fprintf(out, "    br %%%u, b%u, b%u\n", b->cond, b->target,
                        b->target_else); break;
            case FE_IR_RET:
                if (b->has_ret_value) fprintf(out, "    ret %%%u\n", b->ret_value);
                else fputs("    ret\n", out);
                break;
            case FE_IR_TRAP:
                fprintf(out, "    trap %s %lu\n", trap_name(b->trap),
                        b->trap_line);
                break;
            }
        }
        fputs("}\n", out);
    }
}
