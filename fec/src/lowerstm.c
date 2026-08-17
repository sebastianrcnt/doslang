#include "lowerpri.h"

void store_into(Lower *L, FeIrPlace dst, Slot value, FeNode *n,
                       unsigned long size)
{
    if (value.type == FE_IR_MEM) {
        if (!value.is_place) { fail(L, "an aggregate value", n); return; }
        fe_ir_copy(L->m, L->b, dst, value.place, size);
        return;
    }
    fe_ir_store(L->m, L->b, dst, as_value(L, value, n), value.type);
}

void lower_return(Lower *L, FeNode *n)
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

void lower_if(Lower *L, FeNode *n)
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

void lower_while(Lower *L, FeNode *n)
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
Slot lower_slice(Lower *L, FeNode *n)
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
void lower_for(Lower *L, FeNode *n)
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
            (void)lower_reserve(L, (void **)&L->vars, &L->var_capacity,
                                L->var_count,
                                (unsigned long)sizeof(LowerVar));
            L->vars[L->var_count].cname = n->cname;
            L->vars[L->var_count].local = counter;
            L->vars[L->var_count].by_address = 0;
            ++L->var_count;
            item = fe_ir_local(L->m, L->fn, FE_IR_PTR, 4, 4, n->aux_text);
            (void)lower_reserve(L, (void **)&L->vars, &L->var_capacity,
                                L->var_count,
                                (unsigned long)sizeof(LowerVar));
            L->vars[L->var_count].cname = n->aux_cname;
            L->vars[L->var_count].local = item;
            L->vars[L->var_count].by_address = 0;
            ++L->var_count;
        } else {
            item = fe_ir_local(L->m, L->fn, FE_IR_PTR, 4, 4, n->text);
            (void)lower_reserve(L, (void **)&L->vars, &L->var_capacity,
                                L->var_count,
                                (unsigned long)sizeof(LowerVar));
            L->vars[L->var_count].cname = n->cname;
            L->vars[L->var_count].local = item;
            L->vars[L->var_count].by_address = 0;
            ++L->var_count;
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
/* The width of a tag: an enum's own, or the byte an optional puts in front. */
FeIrType tag_type_of(const FeType *t)
{
    if (!t) return FE_IR_I8;
    if (t->kind == FE_TYPE_ERROR_UNION) return FE_IR_I16;
    if (t->kind == FE_TYPE_ENUM) return t->bits > 8U ? FE_IR_I16 : FE_IR_I8;
    return FE_IR_I8;
}

/* Give an arm's names somewhere to live and put the variant's payload there.
   The payload is copied rather than pointed at: an arm that takes ownership of
   what it matched is the normal case, and the checker has already decided
   whether that was allowed. */
void bind_payload(Lower *L, Slot subject, const FeType *t,
                  const FeVariantType *v, FeNode *arm)
{
    FeNode *name;
    unsigned i;
    long base;
    if (!v || !v->field_count || !arm->children || !subject.is_place) return;
    base = (long)fe_type_payload_offset(t);
    name = arm->children;
    for (i = 0; i < v->field_count && name; ++i, name = name->next) {
        FeType *ft = v->fields[i].type;
        unsigned local = declare_var(L, name->cname, ft, name->text);
        FeIrPlace from = subject.place;
        from.offset += base + (long)v->fields[i].offset;
        store_into(L, fe_ir_at_local(local, 0),
                   slot_place(from, ir_type(ft), ir_size(ft)), name,
                   ir_size(ft));
    }
}

/* `if let Some(x) = opt { .. } else { .. }` -- and its None twin.

   The optional is read once into a place, the tag decides the branch, and the
   binding gets what was inside. A binding whose type is a reference gets the
   address instead of a copy: the checker chose that when the payload was not
   something you may quietly duplicate. */
void lower_if_let(Lower *L, FeNode *n)
{
    FeType *opt = n->a ? n->a->sem_type : 0;
    Slot value = lower_expr(L, n->a);
    FeNode *binding = n->children;
    int is_some = n->aux_text && !strcmp(n->aux_text, "Some");
    unsigned tag;
    FeIrBlock *present;
    FeIrBlock *absent;
    FeIrBlock *join;
    if (!value.is_place) { fail(L, "if let over a temporary", n); return; }
    tag = wrapper_tag(L, value, opt, n);
    present = new_block(L);
    absent = new_block(L);
    join = new_block(L);
    fe_ir_br(L->b, tag, present->id, absent->id);
    /* Which side runs the body depends on which pattern was written. */
    L->b = is_some ? present : absent;
    if (is_some && binding) {
        FeType *bt = binding->sem_type;
        Slot payload = wrapper_payload(L, value, opt);
        unsigned local = declare_var(L, binding->cname, bt, binding->text);
        if (bt && (bt->kind == FE_TYPE_REF || bt->kind == FE_TYPE_RAW))
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0),
                        as_address(L, payload, n), FE_IR_PTR);
        else
            store_into(L, fe_ir_at_local(local, 0), payload, n, ir_size(bt));
    }
    lower_stmt(L, n->b);
    fe_ir_jmp(L->b, join->id);
    L->b = is_some ? absent : present;
    if (n->c) lower_stmt(L, n->c);
    fe_ir_jmp(L->b, join->id);
    L->b = join;
}

void lower_match(Lower *L, FeNode *n)
{
    FeType *t = n->a ? n->a->sem_type : 0;
    FeIrType it = ir_type(t);
    Slot subject = lower_expr(L, n->a);
    unsigned value;
    FeIrBlock *join;
    FeNode *arm;
    /* A variant that carries something is memory: the tag comes first and the
       payload after it. Reading the tag is then the same question either way,
       just from a different place. */
    if (it == FE_IR_MEM) {
        if (!subject.is_place) { fail(L, "a match over a temporary", n); return; }
        it = tag_type_of(t);
        value = fe_ir_load(L->m, L->b, it, subject.place);
    } else {
        value = as_value(L, subject, n->a);
    }
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
        bind_payload(L, subject, t, v, arm);
        lower_stmt(L, arm->a);
        fe_ir_jmp(L->b, join->id);
        L->b = next;
    }
    fe_ir_jmp(L->b, join->id);
    L->b = join;
}

void lower_stmt(Lower *L, FeNode *n)
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
        /* `undefined` says the storage starts out unset, so there is nothing
           to write into it. */
        if (n->b && n->b->kind == FE_N_LITERAL && n->b->text &&
            !strcmp(n->b->text, "undefined")) return;
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
        if (n->text && !strcmp(n->text, "if let")) { lower_if_let(L, n); return; }
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
        if (lower_reserve(L, (void **)&L->owed, &L->owed_capacity,
                          L->owed_count, (unsigned long)sizeof *L->owed)) {
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
void lower_global(Lower *L, FeNode *n)
{
    FeType *t = n->sem_type;
    unsigned char *init = 0;
    unsigned long size = ir_size(t);
    if (!n->cname) return;
    /* A text constant is a pointer and a length. The pointer is not a number
       anyone knows yet, so the bytes carry a hole and the linker fills it. */
    if (n->b && n->b->kind == FE_N_LITERAL && n->b->text &&
        n->b->text[0] == '"' && t &&
        (t->kind == FE_TYPE_SLICE || t->kind == FE_TYPE_STR)) {
        char text[1024];
        unsigned long raw = strlen(n->b->text);
        unsigned long len = 0;
        unsigned long i;
        const char *label;
        FeIrGlobal *g;
        if (raw >= 2) raw -= 2;
        for (i = 0; i < raw && len + 1 < sizeof text; ++i) {
            char ch = n->b->text[1 + i];
            if (ch == 92 && i + 1 < raw) {
                ++i;
                switch (n->b->text[1 + i]) {
                case 'n': ch = 10; break;
                case 't': ch = 9; break;
                case 'r': ch = 13; break;
                case '0': ch = 0; break;
                default:  ch = n->b->text[1 + i]; break;
                }
            }
            text[len++] = ch;
        }
        label = fe_ir_string(L->m, text, len);
        init = (unsigned char *)fe_arena_alloc(&L->m->arena, 8);
        if (!init || !label) return;
        for (i = 0; i < 8; ++i) init[i] = 0;
        for (i = 0; i < 4; ++i) init[4 + i] = (unsigned char)((len >> (i * 8)) & 0xFF);
        g = fe_ir_global(L->m, n->cname, FE_IR_MEM, 8, 4, init);
        fe_ir_global_ref(L->m, g, (unsigned long)SLICE_PTR_OFFSET, label);
        return;
    }
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

/* A declaration with type parameters is a pattern, not code. */
int struct_is_generic(const FeNode *decl)
{
    return decl && decl->a && decl->a->children != 0;
}

int fn_is_generic(const FeNode *fn)
{
    FeNode *p;
    if (!fn) return 0;
    for (p = fn->a ? fn->a->children : 0; p; p = p->next)
        if (p->flags & FE_NODE_COMPTIME) return 1;
    return 0;
}

void lower_fn_as(Lower *L, FeNode *fn, const char *name)
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
        if (lower_reserve(L, (void **)&L->vars, &L->var_capacity,
                          L->var_count, (unsigned long)sizeof(LowerVar))) {
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

void lower_fn(Lower *L, FeNode *fn)
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
        for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next)
            if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST)
                lower_global(&L, n);
            else if (n->kind == FE_N_STRUCT && !struct_is_generic(n)) {
                /* A method is a function whose first parameter is the value it
                   was reached through; the storage is the same either way. */
                FeNode *m;
                for (m = n->children; m; m = m->next)
                    if (m->kind == FE_N_FN && m->c) lower_fn(&L, m);
            }
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
