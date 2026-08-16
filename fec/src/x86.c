#include "x86.h"
#include <string.h>

/* ------------------------------------------------------------------------- *
 * i386 code generation
 *
 * The frame, from EBP downwards:
 *
 *     [ebp + 8 + 4k]   incoming argument k
 *     [ebp + 4]        return address
 *     [ebp]            saved ebp
 *     [ebp - ...]      parameters, copied in from the argument area
 *     [ebp - ...]      locals
 *     [ebp - ...]      one slot per temporary
 *
 * Parameters are copied into the frame rather than read in place so that a
 * parameter and a local are the same thing to everything below.
 * ------------------------------------------------------------------------- */

typedef struct Frame {
    const FeIrFunc *f;
    long *local_off;         /* [ebp + off] for each local */
    long temp_base;          /* first temporary slot */
    long size;               /* bytes to subtract from esp */
} Frame;

static long align_up(long v, long a)
{
    long r = v % a;
    return r ? v + a - r : v;
}

static unsigned long slot_bytes(const FeIrLocal *l)
{
    switch (l->type) {
    case FE_IR_I8:  return 1;
    case FE_IR_I16: return 2;
    case FE_IR_I32: return 4;
    case FE_IR_PTR: return 4;
    case FE_IR_MEM: return l->size ? l->size : 1;
    default:        return 4;
    }
}

/* Every temporary is four bytes: a temporary only ever holds something that
   fits in a register, and narrower values are kept zero- or sign-extended. */
#define TEMP_SLOT 4L

static void frame_layout(Frame *fr, const FeIrFunc *f, long *storage)
{
    unsigned i;
    long off = 0;
    fr->f = f;
    fr->local_off = storage;
    for (i = 0; i < f->local_count; ++i) {
        unsigned long size = slot_bytes(&f->locals[i]);
        long a = (long)f->locals[i].align;
        if (a < 1) a = 1;
        if (a > 4) a = 4;
        off = align_up(off + (long)size, a);
        storage[i] = -off;
    }
    off = align_up(off, 4);
    fr->temp_base = -off;
    off += (long)f->temp_count * TEMP_SLOT;
    fr->size = align_up(off, 4);
}

static long temp_off(const Frame *fr, unsigned t)
{
    return fr->temp_base - (long)(t + 1) * TEMP_SLOT;
}

static const char *word_of(FeIrType t)
{
    switch (t) {
    case FE_IR_I8:  return "byte ptr";
    case FE_IR_I16: return "word ptr";
    default:        return "dword ptr";
    }
}

static const char *reg_of(FeIrType t, int which)
{
    /* which: 0 -> a, 1 -> c, 2 -> d */
    switch (t) {
    case FE_IR_I8:  return which == 0 ? "al" : which == 1 ? "cl" : "dl";
    case FE_IR_I16: return which == 0 ? "ax" : which == 1 ? "cx" : "dx";
    default:        return which == 0 ? "eax" : which == 1 ? "ecx" : "edx";
    }
}

/* Write the effective address of a place into `buf`. A place is a base plus a
   constant, and the only base that is not already an address is a temporary,
   which holds a pointer. */
static void place_addr(const Frame *fr, const FeIrPlace *p, char *buf)
{
    switch (p->base) {
    case FE_PLACE_LOCAL:
        sprintf(buf, "[ebp%+ld]", fr->local_off[p->index] + p->offset);
        break;
    case FE_PLACE_GLOBAL:
        if (p->offset) sprintf(buf, "[%s%+ld]", p->name, p->offset);
        else sprintf(buf, "[%s]", p->name);
        break;
    case FE_PLACE_TEMP:
        sprintf(buf, "[edx%+ld]", p->offset);
        break;
    }
}

/* A temporary-based place needs its pointer in a register first. */
static void load_place_base(const Frame *fr, const FeIrPlace *p, FILE *out)
{
    if (p->base != FE_PLACE_TEMP) return;
    fprintf(out, "        mov     edx, [ebp%+ld]\n", temp_off(fr, p->index));
}

static void load_temp(const Frame *fr, unsigned t, const char *reg, FILE *out)
{
    fprintf(out, "        mov     %s, [ebp%+ld]\n", reg, temp_off(fr, t));
}

static void store_temp(const Frame *fr, unsigned t, const char *reg, FILE *out)
{
    fprintf(out, "        mov     [ebp%+ld], %s\n", temp_off(fr, t), reg);
}

static const char *cmp_set(FeIrOp op, int is_unsigned)
{
    switch (op) {
    case FE_IR_EQ: return "sete";
    case FE_IR_NE: return "setne";
    case FE_IR_LT: return is_unsigned ? "setb"  : "setl";
    case FE_IR_LE: return is_unsigned ? "setbe" : "setle";
    case FE_IR_GT: return is_unsigned ? "seta"  : "setg";
    case FE_IR_GE: return is_unsigned ? "setae" : "setge";
    default:       return "sete";
    }
}

static void emit_binary(const Frame *fr, const FeIrValue *v, FILE *out)
{
    int is_cmp = v->op >= FE_IR_EQ && v->op <= FE_IR_GE;
    FeIrType t = is_cmp ? (FeIrType)v->imm : v->type;
    const char *a = reg_of(t, 0);
    const char *c = reg_of(t, 1);
    load_temp(fr, v->a, "eax", out);
    load_temp(fr, v->b, "ecx", out);
    if (is_cmp) {
        fprintf(out, "        cmp     %s, %s\n", a, c);
        fprintf(out, "        %s   al\n", cmp_set(v->op, v->is_unsigned));
        fprintf(out, "        movzx   eax, al\n");
        store_temp(fr, v->dest, "eax", out);
        return;
    }
    switch (v->op) {
    case FE_IR_ADD: fprintf(out, "        add     %s, %s\n", a, c); break;
    case FE_IR_SUB: fprintf(out, "        sub     %s, %s\n", a, c); break;
    case FE_IR_MUL: fprintf(out, "        imul    %s, %s\n", a, c); break;
    case FE_IR_AND: fprintf(out, "        and     %s, %s\n", a, c); break;
    case FE_IR_OR:  fprintf(out, "        or      %s, %s\n", a, c); break;
    case FE_IR_XOR: fprintf(out, "        xor     %s, %s\n", a, c); break;
    case FE_IR_SHL: fprintf(out, "        shl     %s, cl\n", a); break;
    case FE_IR_SHR:
        fprintf(out, "        %s     %s, cl\n",
                v->is_unsigned ? "shr" : "sar", a);
        break;
    case FE_IR_DIV:
    case FE_IR_MOD:
        /* The divide instructions use edx:eax, so the operands have to be
           widened to 32 bits whatever the declared width is. */
        if (v->is_unsigned) fprintf(out, "        xor     edx, edx\n");
        else fprintf(out, "        cdq\n");
        fprintf(out, "        %s     ecx\n", v->is_unsigned ? "div " : "idiv");
        if (v->op == FE_IR_MOD) fprintf(out, "        mov     eax, edx\n");
        break;
    default: break;
    }
    store_temp(fr, v->dest, "eax", out);
}

static void emit_value(const Frame *fr, const FeIrValue *v, FILE *out)
{
    char addr[128];
    unsigned i;
    switch (v->op) {
    case FE_IR_CONST:
        fprintf(out, "        mov     eax, %ld\n", v->imm);
        store_temp(fr, v->dest, "eax", out);
        break;
    case FE_IR_LOAD:
        load_place_base(fr, &v->place, out);
        place_addr(fr, &v->place, addr);
        if (v->type == FE_IR_I8)
            fprintf(out, "        movzx   eax, byte ptr %s\n", addr);
        else if (v->type == FE_IR_I16)
            fprintf(out, "        movzx   eax, word ptr %s\n", addr);
        else
            fprintf(out, "        mov     eax, dword ptr %s\n", addr);
        store_temp(fr, v->dest, "eax", out);
        break;
    case FE_IR_STORE:
        load_place_base(fr, &v->place, out);
        place_addr(fr, &v->place, addr);
        load_temp(fr, v->a, "eax", out);
        fprintf(out, "        mov     %s %s, %s\n", word_of(v->type), addr,
                reg_of(v->type, 0));
        break;
    case FE_IR_ADDR:
        load_place_base(fr, &v->place, out);
        place_addr(fr, &v->place, addr);
        fprintf(out, "        lea     eax, %s\n", addr);
        store_temp(fr, v->dest, "eax", out);
        break;
    case FE_IR_CAST:
        load_temp(fr, v->a, "eax", out);
        /* Narrowing is free once everything is kept in a 32-bit slot; widening
           has to say whether the top bits are copies of the sign. */
        if (v->type == FE_IR_I8)
            fprintf(out, "        %s   eax, al\n",
                    v->is_unsigned ? "movzx" : "movsx");
        else if (v->type == FE_IR_I16)
            fprintf(out, "        %s   eax, ax\n",
                    v->is_unsigned ? "movzx" : "movsx");
        store_temp(fr, v->dest, "eax", out);
        break;
    case FE_IR_CALL:
        /* cdecl: arguments pushed right to left, the caller pops them. */
        for (i = v->arg_count; i > 0; --i) {
            load_temp(fr, v->args[i - 1], "eax", out);
            fprintf(out, "        push    eax\n");
        }
        fprintf(out, "        call    %s\n", v->callee);
        if (v->arg_count)
            fprintf(out, "        add     esp, %u\n", v->arg_count * 4U);
        if (v->has_dest) store_temp(fr, v->dest, "eax", out);
        break;
    case FE_IR_COPY: {
        char dst[128];
        char src[128];
        /* The source base and the destination base both want edx, so a
           temporary-based place is resolved into esi or edi first. */
        if (v->place2.base == FE_PLACE_TEMP) {
            load_temp(fr, v->place2.index, "esi", out);
            sprintf(src, "[esi%+ld]", v->place2.offset);
        } else {
            place_addr(fr, &v->place2, src);
        }
        if (v->place.base == FE_PLACE_TEMP) {
            load_temp(fr, v->place.index, "edi", out);
            sprintf(dst, "[edi%+ld]", v->place.offset);
        } else {
            place_addr(fr, &v->place, dst);
        }
        fprintf(out, "        lea     esi, %s\n", src);
        fprintf(out, "        lea     edi, %s\n", dst);
        fprintf(out, "        mov     ecx, %ld\n", v->imm);
        fprintf(out, "        cld\n");
        fprintf(out, "        rep movsb\n");
        break;
    }
    default:
        emit_binary(fr, v, out);
        break;
    }
}

static void emit_func(const FeIrModule *m, const FeIrFunc *f, FILE *out)
{
    Frame fr;
    long storage[512];
    const FeIrBlock *b;
    const FeIrValue *v;
    unsigned i;
    long arg = 8;
    if (f->is_extern || !f->first) return;
    if (f->local_count > 512) return;
    frame_layout(&fr, f, storage);

    fprintf(out, "\npublic %s\n", f->name);
    fprintf(out, "%s proc near\n", f->name);
    fprintf(out, "        push    ebp\n");
    fprintf(out, "        mov     ebp, esp\n");
    if (fr.size) fprintf(out, "        sub     esp, %ld\n", fr.size);
    fprintf(out, "        push    esi\n        push    edi\n");
    /* Copy the incoming arguments into the frame. */
    for (i = 0; i < f->param_count; ++i) {
        fprintf(out, "        mov     eax, [ebp+%ld]\n", arg);
        fprintf(out, "        mov     %s [ebp%+ld], %s\n",
                word_of(f->locals[i].type), storage[i],
                reg_of(f->locals[i].type, 0));
        arg += 4;
    }

    for (b = f->first; b; b = b->next) {
        fprintf(out, "L%s_%u:\n", f->name, b->id);
        for (v = b->first; v; v = v->next) emit_value(&fr, v, out);
        switch (b->term) {
        case FE_IR_JMP:
            fprintf(out, "        jmp     L%s_%u\n", f->name, b->target);
            break;
        case FE_IR_BR:
            load_temp(&fr, b->cond, "eax", out);
            fprintf(out, "        test    eax, eax\n");
            fprintf(out, "        jnz     L%s_%u\n", f->name, b->target);
            fprintf(out, "        jmp     L%s_%u\n", f->name, b->target_else);
            break;
        case FE_IR_RET:
            if (b->has_ret_value) load_temp(&fr, b->ret_value, "eax", out);
            fprintf(out, "        pop     edi\n        pop     esi\n");
            fprintf(out, "        mov     esp, ebp\n        pop     ebp\n");
            fprintf(out, "        ret\n");
            break;
        case FE_IR_TRAP:
            fprintf(out, "        push    %lu\n", b->trap_line);
            fprintf(out, "        push    offset FE_UNIT_FILE\n");
            fprintf(out, "        push    %u\n", (unsigned)b->trap);
            fprintf(out, "        call    fe_trap\n");
            fprintf(out, "        add     esp, 12\n");
            break;
        }
    }
    fprintf(out, "%s endp\n", f->name);
    (void)m;
}

static void emit_string(const char *s, FILE *out)
{
    int in = 0;
    fputs("        db      ", out);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c >= 32 && c < 127 && c != '\'' && c != '"') {
            if (!in) { fputc('\'', out); in = 1; }
            fputc(c, out);
        } else {
            if (in) { fputs("',", out); in = 0; }
            fprintf(out, "%u,", c);
        }
    }
    if (in) fputc('\'', out);
    else fputc('0', out);
    if (in) fputs(",0", out);
    fputc('\n', out);
}

void fe_x86_emit(const FeIrModule *m, FILE *out)
{
    const FeIrFunc *f;
    const FeIrGlobal *g;
    int any_trap = 0;
    const FeIrBlock *b;

    for (f = m->funcs; f && !any_trap; f = f->next)
        for (b = f->first; b; b = b->next)
            if (b->term == FE_IR_TRAP) { any_trap = 1; break; }

    fputs(".386\n.model flat\n\n", out);
    for (f = m->funcs; f; f = f->next)
        if (f->is_extern || !f->first)
            fprintf(out, "extern %s : near\n", f->name);
    /* Anything called but not defined here lives somewhere else -- the runtime,
       or a library. Lowering emits such calls directly (allocating, writing,
       trapping), so the names are collected from the calls themselves rather
       than from a list that would have to be kept in step. */
    {
        const char *seen[64];
        unsigned count = 0;
        const FeIrValue *v;
        const FeIrFunc *g;
        unsigned i;
        for (f = m->funcs; f; f = f->next)
            for (b = f->first; b; b = b->next)
                for (v = b->first; v; v = v->next) {
                    if (v->op != FE_IR_CALL || !v->callee) continue;
                    for (g = m->funcs; g; g = g->next)
                        if (!strcmp(g->name, v->callee)) break;
                    if (g) continue;
                    for (i = 0; i < count; ++i)
                        if (!strcmp(seen[i], v->callee)) break;
                    if (i < count || count >= 64) continue;
                    seen[count++] = v->callee;
                    fprintf(out, "extern %s : near\n", v->callee);
                }
    }
    if (any_trap) fputs("extern fe_trap : near\n", out);

    fputs("\n_DATA segment dword public 'DATA'\n", out);
    if (any_trap) {
        fputs("public FE_UNIT_FILE\nFE_UNIT_FILE label byte\n", out);
        emit_string(m->unit_file, out);
    }
    for (g = m->globals; g; g = g->next) {
        unsigned long i;
        fprintf(out, "public %s\n%s label byte\n", g->name, g->name);
        if (!g->init) {
            fprintf(out, "        db      %lu dup(0)\n", g->size ? g->size : 1UL);
            continue;
        }
        for (i = 0; i < g->size; ++i) {
            if (i % 16 == 0) fputs("        db      ", out);
            fprintf(out, "%u%s", g->init[i],
                    (i + 1 == g->size || (i % 16) == 15) ? "\n" : ",");
        }
        if (!g->size) fputs("        db      0\n", out);
    }
    fputs("_DATA ends\n", out);

    fputs("\n_TEXT segment dword public 'CODE'\n", out);
    for (f = m->funcs; f; f = f->next) emit_func(m, f, out);
    /* The runtime's entry stub calls one fixed name, so point it here. */
    if (m->entry_main)
        fprintf(out, "\npublic fe_main_\nfe_main_ proc near\n"
                     "        jmp     %s\nfe_main_ endp\n", m->entry_main);
    fputs("\n_TEXT ends\n\nend\n", out);
}
