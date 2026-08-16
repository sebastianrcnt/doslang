#include "lowerpri.h"

void emit_text(Lower *L, unsigned handle, const char *text,
                      unsigned long len)
{
    unsigned args[3];
    const char *label;
    if (!len) return;
    label = fe_ir_string(L->m, text, len);
    if (!label) return;
    args[0] = fe_ir_const(L->m, L->b, FE_IR_I32, (long)handle);
    args[1] = fe_ir_addr(L->m, L->b, fe_ir_at_global(label, 0));
    args[2] = fe_ir_const(L->m, L->b, FE_IR_I32, (long)len);
    fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_write", args, 3);
}

/* Write one value, the way the verb asked for. */
void emit_value_text(Lower *L, unsigned handle, FeNode *arg, int verb)
{
    FeType *t = arg ? arg->sem_type : 0;
    Slot v = lower_expr(L, arg);
    unsigned args[3];
    if (t && (t->kind == FE_TYPE_SLICE || t->kind == FE_TYPE_STR)) {
        FeIrPlace at = v.place;
        if (!v.is_place) { fail(L, "text with no place", arg); return; }
        args[0] = fe_ir_const(L->m, L->b, FE_IR_I32, (long)handle);
        at.offset = v.place.offset + SLICE_PTR_OFFSET;
        args[1] = fe_ir_load(L->m, L->b, FE_IR_PTR, at);
        at.offset = v.place.offset + SLICE_LEN_OFFSET;
        args[2] = fe_ir_load(L->m, L->b, FE_IR_I32, at);
        fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_write", args, 3);
        return;
    }
    if (t && t->kind == FE_TYPE_BOOL) {
        /* Two literals and a branch: cheaper than a runtime that knows about
           Ferro's names for truth. */
        FeIrBlock *yes = new_block(L);
        FeIrBlock *no = new_block(L);
        FeIrBlock *join = new_block(L);
        fe_ir_br(L->b, as_value(L, v, arg), yes->id, no->id);
        L->b = yes;
        emit_text(L, handle, "true", 4);
        fe_ir_jmp(L->b, join->id);
        L->b = no;
        emit_text(L, handle, "false", 5);
        fe_ir_jmp(L->b, join->id);
        L->b = join;
        return;
    }
    if (verb == 'c' || (t && t->kind == FE_TYPE_CHAR)) {
        /* One byte, written from a slot of its own so it has an address. */
        unsigned cell = fe_ir_local(L->m, L->fn, FE_IR_I8, 1, 1, "char");
        fe_ir_store(L->m, L->b, fe_ir_at_local(cell, 0), as_value(L, v, arg),
                    FE_IR_I8);
        args[0] = fe_ir_const(L->m, L->b, FE_IR_I32, (long)handle);
        args[1] = fe_ir_addr(L->m, L->b, fe_ir_at_local(cell, 0));
        args[2] = fe_ir_const(L->m, L->b, FE_IR_I32, 1);
        fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_write", args, 3);
        return;
    }
    args[0] = fe_ir_const(L->m, L->b, FE_IR_I32, (long)handle);
    args[1] = as_value(L, v, arg);
    if (verb == 'x') {
        fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_write_hex", args, 2);
        return;
    }
    args[2] = fe_ir_const(L->m, L->b, FE_IR_I32,
                          type_is_unsigned(t) || (t && t->kind == FE_TYPE_ENUM)
                          ? 1 : 0);
    fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_write_int", args, 3);
}

/* `@print(fmt, ...)`, `@fprint(w, fmt, ...)`. The checker has already agreed
   that the string is a literal and that the count matches. */
int lower_print(Lower *L, FeNode *n, Slot *out)
{
    const char *name = n->text;
    int to_writer;
    unsigned handle;
    FeNode *fmt;
    FeNode *arg;
    const char *text;
    unsigned long raw;
    unsigned long i;
    unsigned long chunk;
    char plain[1024];
    unsigned long plain_len;
    if (!name || (strcmp(name, "@print") != 0 && strcmp(name, "@fprint") != 0))
        return 0;
    to_writer = strcmp(name, "@fprint") == 0;
    fmt = n->children;
    if (to_writer) {
        /* A Writer is a handle; Stdout is 1 and Stderr is 2 (std.io). */
        Slot w = lower_expr(L, fmt);
        handle = 0;
        (void)w;
        fmt = fmt ? fmt->next : 0;
    }
    handle = to_writer ? 2 : 1;
    if (!fmt || !fmt->text || fmt->text[0] != '"') {
        fail(L, "a format string that is not a literal", n);
        *out = slot_void();
        return 1;
    }
    text = fmt->text + 1;
    raw = strlen(fmt->text);
    if (raw >= 2) raw -= 2;
    arg = fmt->next;
    plain_len = 0;
    chunk = 0;
    (void)chunk;
    for (i = 0; i < raw; ++i) {
        char ch = text[i];
        if (ch == 92 && i + 1 < raw) {           /* an escape */
            ++i;
            switch (text[i]) {
            case 'n': ch = 10; break;
            case 't': ch = 9; break;
            case 'r': ch = 13; break;
            case '0': ch = 0; break;
            default:  ch = text[i]; break;
            }
            if (plain_len + 1 < sizeof plain) plain[plain_len++] = ch;
            continue;
        }
        if (ch == '{') {
            int verb = ' ';
            unsigned long close = i + 1;
            while (close < raw && text[close] != '}') ++close;
            if (close == i + 2) verb = text[i + 1];
            emit_text(L, handle, plain, plain_len);
            plain_len = 0;
            emit_value_text(L, handle, arg, verb);
            if (arg) arg = arg->next;
            i = close;
            continue;
        }
        if (ch == '}') continue;                 /* `}}` is one brace */
        if (plain_len + 1 < sizeof plain) plain[plain_len++] = ch;
    }
    emit_text(L, handle, plain, plain_len);
    *out = slot_void();
    return 1;
}

Slot lower_call(Lower *L, FeNode *n)
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
        if (lower_print(L, n, &built)) return built;
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


/* Every expression may be standing where a wrapper is expected, so the wrap is
   applied once, here, rather than at each place that could need it. */
Slot lower_expr(Lower *L, FeNode *n)
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
