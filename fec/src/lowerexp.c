#include "lowerpri.h"

Slot lower_expr_core(Lower *L, FeNode *n)
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
        /* Comparing an optional with `null` asks about its tag, not about the
           bytes of the whole wrapper -- which has no value form at all. */
        if ((op == FE_IR_EQ || op == FE_IR_NE) && n->a && n->b) {
            FeNode *w = fe_m7_is_null(n->b) ? n->a :
                        (fe_m7_is_null(n->a) ? n->b : 0);
            FeType *wt = w ? w->sem_type : 0;
            if (wt && wt->kind == FE_TYPE_OPTIONAL) {
                Slot s = lower_expr(L, w);
                unsigned t0;
                unsigned z;
                if (!s.is_place) {
                    fail(L, "an optional with no place", w);
                    return slot_void();
                }
                t0 = wrapper_tag(L, s, wt, w);
                z = fe_ir_const(L->m, L->b,
                                uses_niche(wt) ? FE_IR_PTR : FE_IR_I8, 0);
                return slot_value(fe_ir_binary(L->m, L->b, op,
                    uses_niche(wt) ? FE_IR_PTR : FE_IR_I8, t0, z, 1),
                    FE_IR_I8);
            }
        }
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
        /* A variant used as a value carries nothing but its tag. When no
           variant of the enum carries anything the whole value is that tag;
           otherwise it is a tag sitting in front of an unused payload. */
        if (t && t->kind == FE_TYPE_ENUM && n->b && n->b->text) {
            FeVariantType *v = fe_type_variant(t, n->b->text);
            if (v && !enum_has_payload(t))
                return slot_value(fe_ir_const(L->m, L->b, ir_type(t),
                                              (long)v->tag), ir_type(t));
            if (v && !v->field_count) {
                unsigned local = scratch(L, t, "variant");
                unsigned tag = fe_ir_const(L->m, L->b, tag_type_of(t),
                                           (long)v->tag);
                fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), tag,
                            tag_type_of(t));
                return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM,
                                  ir_size(t));
            }
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
           compile time and a slice carries beside its pointer. Only for those:
           a struct is free to have a field called `n`, and reading it as a
           length would quietly hand back the wrong four bytes. */
        if (n->b && n->b->text && !strcmp(n->b->text, "n") &&
            n->a && n->a->sem_type &&
            (n->a->sem_type->kind == FE_TYPE_ARRAY ||
             n->a->sem_type->kind == FE_TYPE_SLICE ||
             n->a->sem_type->kind == FE_TYPE_STR)) {
            FeType *bt = n->a->sem_type;
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
            /* `binding.name` is not a field of anything: it is a constant or a
               global in another unit, and the checker already turned it into a
               link name. */
            if (!field && n->cname)
                return slot_place(fe_ir_at_global(n->cname, 0), it,
                                  ir_size(t));
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
        unsigned local;
        FeNode *f;
        /* `Enum.Variant{ .. }` builds a variant, not a struct: the tag first,
           then the named fields inside the payload area. */
        if (t && t->kind == FE_TYPE_ENUM && n->a && n->a->kind == FE_N_MEMBER) {
            const FeVariantType *v = fe_type_variant(t,
                n->a->b && n->a->b->text ? n->a->b->text : "");
            long base = (long)fe_type_payload_offset(t);
            unsigned tag;
            if (!v) { fail(L, "an unknown variant", n); return slot_void(); }
            local = scratch(L, t, "variant");
            tag = fe_ir_const(L->m, L->b, tag_type_of(t), (long)v->tag);
            fe_ir_store(L->m, L->b, fe_ir_at_local(local, 0), tag,
                        tag_type_of(t));
            for (f = n->children; f; f = f->next) {
                unsigned i;
                if (f->kind != FE_N_FIELD) continue;
                for (i = 0; i < v->field_count; ++i)
                    if (f->text && v->fields[i].name &&
                        !strcmp(v->fields[i].name, f->text)) break;
                if (i == v->field_count) {
                    fail(L, "an unknown variant field", f);
                    return slot_void();
                }
                store_into(L, fe_ir_at_local(local,
                                             base + (long)v->fields[i].offset),
                           lower_expr(L, f->a), f, ir_size(v->fields[i].type));
            }
            return slot_place(fe_ir_at_local(local, 0), FE_IR_MEM, ir_size(t));
        }
        local = scratch(L, t, "struct");
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
const char *drop_name(Lower *L, const FeType *t)
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

/* Let go of one value sitting at `at`. A type that says how to let go of
   itself is asked first; then whatever it holds is let go of in turn, so a
   struct that owns a struct that owns a buffer settles all three without
   anyone writing a `drop` (SPEC 5 R1). */
void release_at(Lower *L, const FeType *t, FeIrPlace at)
{
    unsigned args[1];
    unsigned i;
    if (!t) return;
    if (t->has_drop) {
        const char *how = drop_name(L, t);
        args[0] = fe_ir_addr(L->m, L->b, at);
        if (how) fe_ir_call(L->m, L->b, FE_IR_VOID, how, args, 1);
    }
    if (t->kind == FE_TYPE_OWNED) {
        FeIrPlace p = at;
        if (t->elem && t->elem->kind == FE_TYPE_SLICE)
            p.offset += SLICE_PTR_OFFSET;
        args[0] = fe_ir_load(L->m, L->b, FE_IR_PTR, p);
        fe_ir_call(L->m, L->b, FE_IR_VOID, "fe_rt_free", args, 1);
        return;
    }
    if (t->kind == FE_TYPE_STRUCT)
        for (i = 0; i < t->field_count; ++i) {
            FeIrPlace p = at;
            if (!needs_release(t->fields[i].type)) continue;
            p.offset += (long)t->fields[i].offset;
            release_at(L, t->fields[i].type, p);
        }
}

/* Settle what a scope owes, most recent first. A `return` in the middle of a
   function still owes everything, so every exit path calls this. */
void run_deferred(Lower *L, unsigned from)
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
            FeType *t = L->owed[i - 1].type;
            fe_ir_br(L->b, live, doit->id, skip->id);
            L->b = doit;
            release_at(L, t, fe_ir_at_local(L->owed[i - 1].local, 0));
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

Slot wrap_context(Lower *L, Slot v, FeNode *n)
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
unsigned wrapper_tag(Lower *L, Slot w, const FeType *t, FeNode *n)
{
    FeIrPlace p;
    if (!w.is_place) { fail(L, "a wrapper with no place", n); return 0; }
    p = w.place;
    if (uses_niche(t)) return fe_ir_load(L->m, L->b, FE_IR_PTR, p);
    return fe_ir_load(L->m, L->b, tag_type(t), p);
}

Slot wrapper_payload(Lower *L, Slot w, const FeType *t)
{
    FeType *payload = t ? (t->kind == FE_TYPE_ERROR_UNION ? t->error_value
                                                          : t->elem) : 0;
    FeIrPlace p = w.place;
    (void)L;
    p.offset += (long)fe_type_payload_offset(t);
    return slot_place(p, ir_type(payload), ir_size(payload));
}

/* Leave the function with this error code, after the deferred blocks. */
void return_error(Lower *L, unsigned err, FeNode *n)
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
Slot lower_try(Lower *L, FeNode *n)
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
Slot lower_lazy(Lower *L, FeNode *n, int is_catch)
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
