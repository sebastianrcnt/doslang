#include "own.h"
#include <string.h>

static FeLoc fe_own_no_loc(void)
{
    FeLoc loc;
    loc.file = 0;
    loc.line = 0;
    loc.col = 0;
    return loc;
}

static void fe_own_error_note(FeDiags *diags, FeLoc loc, const char *msg,
                              FeLoc note, const char *note_msg)
{
    fe_diag_error(diags, loc, msg);
    if (note.file) fe_diag_note_src(diags, note, note_msg);
}

int fe_own_is_copy_type(FeType *type)
{
    unsigned i;
    if (!type) return 1;
    if (type->kind == FE_TYPE_OWNED) return 0;
    if (type->kind == FE_TYPE_REF || type->kind == FE_TYPE_SLICE)
        return !type->ref_mut;
    if (type->kind == FE_TYPE_OPTIONAL)
        return fe_own_is_copy_type(type->elem);
    if (type->kind == FE_TYPE_ERROR_UNION)
        return fe_own_is_copy_type(type->error_value);
    if (type->kind == FE_TYPE_ARRAY)
        return fe_own_is_copy_type(type->elem);
    if (type->kind == FE_TYPE_STRUCT) {
        if (type->has_drop) return 0;
        for (i = 0; i < type->field_count; ++i)
            if (!fe_own_is_copy_type(type->fields[i].type)) return 0;
    }
    if (type->kind == FE_TYPE_ENUM) {
        for (i = 0; i < type->variant_count; ++i) {
            unsigned j;
            for (j = 0; j < type->variants[i].field_count; ++j)
                if (!fe_own_is_copy_type(type->variants[i].fields[j].type))
                    return 0;
        }
    }
    return 1;
}

int fe_own_is_reference_like(FeType *type)
{
    return type && (type->kind == FE_TYPE_REF ||
                    type->kind == FE_TYPE_SLICE ||
                    type->kind == FE_TYPE_STR);
}

static FeNode *fe_own_root_expr(FeNode *expr)
{
    if (!expr) return 0;
    if (expr->kind == FE_N_IDENT) return expr;
    if (expr->kind == FE_N_MEMBER || expr->kind == FE_N_INDEX)
        return fe_own_root_expr(expr->a);
    if (expr->kind == FE_N_UNARY && expr->text &&
        (strcmp(expr->text, "&") == 0 || strcmp(expr->text, "&mut") == 0))
        return fe_own_root_expr(expr->a);
    return 0;
}

static int fe_own_expr_has_projection(FeNode *expr)
{
    if (!expr) return 0;
    if (expr->kind == FE_N_MEMBER || expr->kind == FE_N_INDEX) return 1;
    if (expr->kind == FE_N_UNARY && expr->text &&
        (strcmp(expr->text, "&") == 0 || strcmp(expr->text, "&mut") == 0))
        return fe_own_expr_has_projection(expr->a);
    return 0;
}

int fe_own_place_from_expr(FeNode *expr, FeOwnPlace *place)
{
    FeNode *root;
    if (!place) return 0;
    place->root = 0;
    place->root_cname = 0;
    place->projected = 0;
    root = fe_own_root_expr(expr);
    if (!root) return 0;
    place->root = root;
    place->root_cname = root->cname ? root->cname : root->text;
    place->projected = fe_own_expr_has_projection(expr);
    return place->root_cname != 0;
}

void fe_own_state_init(FeOwnState *state, int initialized)
{
    if (!state) return;
    state->move = FE_OWN_AVAILABLE;
    state->initialized = initialized != 0;
    state->shared = 0;
    state->exclusive = 0;
    state->borrow_conflict = 0;
    state->move_loc = fe_own_no_loc();
    state->borrow_loc = fe_own_no_loc();
}

static int fe_own_require_value(FeDiags *diags, FeOwnState *state, FeLoc loc)
{
    if (state->move == FE_OWN_MOVED) {
        fe_own_error_note(diags, loc, "use of moved value", state->move_loc,
                          "value was moved here");
        return 0;
    }
    if (state->move == FE_OWN_MAYBE_MOVED) {
        fe_diag_error(diags, loc, "use of possibly moved value");
        return 0;
    }
    if (!state->initialized) {
        fe_diag_error(diags, loc, "use of uninitialized variable");
        return 0;
    }
    return 1;
}

static int fe_own_require_stable_borrow(FeDiags *diags, FeOwnState *state,
                                        FeLoc loc)
{
    if (!state->borrow_conflict) return 1;
    fe_own_error_note(diags, loc,
        "incompatible borrow state across control-flow paths",
        state->borrow_loc, "borrow originated here");
    return 0;
}

int fe_own_access(FeDiags *diags, FeOwnState *state,
                  FeOwnAccessKind access, FeLoc loc)
{
    if (!state) return 0;
    if (access == FE_OWN_PROJECTION) return 1;

    if (!fe_own_require_stable_borrow(diags, state, loc)) return 0;

    if (access == FE_OWN_WRITE) {
        if (state->shared || state->exclusive) {
            fe_own_error_note(diags, loc, "cannot write while value is borrowed",
                              state->borrow_loc, "borrow originated here");
            return 0;
        }
        state->move = FE_OWN_AVAILABLE;
        state->initialized = 1;
        state->move_loc = fe_own_no_loc();
        return 1;
    }

    if (!fe_own_require_value(diags, state, loc)) return 0;

    switch (access) {
    case FE_OWN_READ:
        if (state->exclusive) {
            fe_own_error_note(diags, loc,
                "cannot read directly while value is mutably borrowed",
                state->borrow_loc, "mutable borrow originated here");
            return 0;
        }
        return 1;
    case FE_OWN_MOVE:
        if (state->shared || state->exclusive) {
            fe_own_error_note(diags, loc, "cannot move while value is borrowed",
                              state->borrow_loc, "borrow originated here");
            return 0;
        }
        state->move = FE_OWN_MOVED;
        state->initialized = 0;
        state->move_loc = loc;
        return 1;
    case FE_OWN_BORROW_SHARED:
        if (state->exclusive) {
            fe_own_error_note(diags, loc,
                "cannot create shared borrow while mutable borrow is live",
                state->borrow_loc, "mutable borrow originated here");
            return 0;
        }
        if (!state->shared) state->borrow_loc = loc;
        ++state->shared;
        return 1;
    case FE_OWN_BORROW_MUT:
        if (state->shared || state->exclusive) {
            fe_own_error_note(diags, loc,
                "cannot create mutable borrow while another borrow is live",
                state->borrow_loc, "existing borrow originated here");
            return 0;
        }
        state->exclusive = 1;
        state->borrow_loc = loc;
        return 1;
    default:
        break;
    }
    return 1;
}

int fe_own_call_shared_view(FeDiags *diags, FeOwnState *state, FeLoc loc)
{
    if (!state) return 0;
    if (!fe_own_require_stable_borrow(diags, state, loc)) return 0;
    if (!fe_own_require_value(diags, state, loc)) return 0;
    if (!state->exclusive) {
        fe_diag_error(diags, loc,
            "read-only reborrow requires a live mutable borrow");
        return 0;
    }
    return 1;
}

void fe_own_release_shared(FeOwnState *state)
{
    if (!state || !state->shared) return;
    --state->shared;
    if (!state->shared && !state->exclusive)
        state->borrow_loc = fe_own_no_loc();
}

void fe_own_release_exclusive(FeOwnState *state)
{
    if (!state) return;
    state->exclusive = 0;
    if (!state->shared) state->borrow_loc = fe_own_no_loc();
}

FeOwnState fe_own_merge_state(FeOwnState left, FeOwnState right)
{
    FeOwnState out;
    out.move = left.move == right.move ? left.move :
        fe_own_merge_move(left.move, right.move);
    out.initialized = left.initialized && right.initialized;
    out.shared = left.shared > right.shared ? left.shared : right.shared;
    out.exclusive = left.exclusive || right.exclusive;
    out.borrow_conflict = left.borrow_conflict || right.borrow_conflict ||
        (out.shared != 0 && out.exclusive != 0);
    out.move_loc = left.move != FE_OWN_AVAILABLE ? left.move_loc : right.move_loc;
    if (left.shared || left.exclusive || left.borrow_conflict)
        out.borrow_loc = left.borrow_loc;
    else
        out.borrow_loc = right.borrow_loc;
    return out;
}

int fe_own_state_equal(const FeOwnState *left, const FeOwnState *right)
{
    if (!left || !right) return 0;
    return left->move == right->move &&
           left->initialized == right->initialized &&
           left->shared == right->shared &&
           left->exclusive == right->exclusive &&
           left->borrow_conflict == right->borrow_conflict;
}

int fe_own_loop_merge_state(FeOwnState entry, FeOwnState backedge,
                            FeOwnState *merged)
{
    if (!merged) return 0;
    *merged = fe_own_merge_state(entry, backedge);
    return fe_own_state_equal(&entry, merged);
}

FeOwnProvenance fe_own_provenance_static(void)
{
    FeOwnProvenance p;
    p.kind = FE_OWN_PROV_STATIC;
    p.param_index = 0;
    return p;
}

FeOwnProvenance fe_own_provenance_param(unsigned param_index)
{
    FeOwnProvenance p;
    p.kind = FE_OWN_PROV_PARAM;
    p.param_index = param_index;
    return p;
}

FeOwnProvenance fe_own_merge_provenance(FeOwnProvenance left,
                                        FeOwnProvenance right)
{
    FeOwnProvenance invalid;
    invalid.kind = FE_OWN_PROV_INVALID;
    invalid.param_index = 0;
    if (left.kind == FE_OWN_PROV_INVALID || right.kind == FE_OWN_PROV_INVALID)
        return invalid;
    if (left.kind == FE_OWN_PROV_STATIC) return right;
    if (right.kind == FE_OWN_PROV_STATIC) return left;
    if (left.param_index == right.param_index) return left;
    return invalid;
}

void fe_own_liveness_init(FeOwnLiveness *live, FeArena *arena)
{
    if (!live) return;
    live->arena = arena;
    live->items = 0;
    live->count = 0;
    live->capacity = 0;
    live->ordinal = 0;
}

static FeOwnLastUse *fe_own_live_find(FeOwnLiveness *live, const char *cname)
{
    unsigned i;
    if (!live || !cname) return 0;
    for (i = 0; i < live->count; ++i)
        if (live->items[i].cname == cname ||
            strcmp(live->items[i].cname, cname) == 0)
            return &live->items[i];
    return 0;
}

static int fe_own_live_add(FeOwnLiveness *live, FeNode *decl)
{
    FeOwnLastUse *items;
    FeOwnLastUse *slot;
    unsigned capacity;
    const char *cname;
    if (!live || !decl || !live->arena)
        return 1;
    cname = decl->cname ? decl->cname : decl->text;
    if (!cname || fe_own_live_find(live, cname)) return 1;
    if (live->count == live->capacity) {
        capacity = live->capacity ? live->capacity * 2U : 8U;
        items = (FeOwnLastUse *)fe_arena_alloc(live->arena,
            capacity * sizeof(FeOwnLastUse));
        if (!items) return 0;
        if (live->items)
            memcpy(items, live->items, live->count * sizeof(FeOwnLastUse));
        live->items = items;
        live->capacity = capacity;
    }
    slot = &live->items[live->count++];
    slot->cname = cname;
    slot->decl = decl;
    slot->last_node = 0;
    slot->last_ordinal = 0;
    slot->defer_extended = 0;
    return 1;
}

static unsigned long fe_own_node_weight(FeNode *node);

static unsigned long fe_own_list_weight(FeNode *node)
{
    unsigned long count;
    count = 0;
    while (node) {
        count += fe_own_node_weight(node);
        node = node->next;
    }
    return count;
}

static unsigned long fe_own_node_weight(FeNode *node)
{
    unsigned long count;
    if (!node) return 0;
    count = 1;
    count += fe_own_node_weight(node->a);
    count += fe_own_node_weight(node->b);
    count += fe_own_node_weight(node->c);
    count += fe_own_list_weight(node->children);
    return count;
}

static int fe_own_live_visit(FeOwnLiveness *live, FeNode *node,
                             unsigned long block_end,
                             unsigned long defer_until);

static int fe_own_live_visit_list(FeOwnLiveness *live, FeNode *node,
                                  unsigned long block_end,
                                  unsigned long defer_until)
{
    while (node) {
        if (!fe_own_live_visit(live, node, block_end, defer_until)) return 0;
        node = node->next;
    }
    return 1;
}

static int fe_own_live_visit(FeOwnLiveness *live, FeNode *node,
                             unsigned long block_end,
                             unsigned long defer_until)
{
    FeOwnLastUse *slot;
    unsigned long end;
    unsigned long effective;
    if (!node) return 1;
    ++live->ordinal;

    if (node->kind == FE_N_IDENT) {
        slot = fe_own_live_find(live, node->cname ? node->cname : node->text);
        if (slot) {
            effective = defer_until ? defer_until : live->ordinal;
            if (effective >= slot->last_ordinal) {
                slot->last_ordinal = effective;
                slot->last_node = node;
                if (defer_until) slot->defer_extended = 1;
            }
        }
        return 1;
    }

    if (node->kind == FE_N_BLOCK) {
        end = live->ordinal + fe_own_list_weight(node->children);
        return fe_own_live_visit_list(live, node->children, end, defer_until);
    }

    if (node->kind == FE_N_DEFER) {
        effective = defer_until ? defer_until : block_end;
        return fe_own_live_visit(live, node->a, block_end, effective);
    }

    if (!fe_own_live_visit(live, node->a, block_end, defer_until)) return 0;
    if (!fe_own_live_visit(live, node->b, block_end, defer_until)) return 0;
    if (!fe_own_live_visit(live, node->c, block_end, defer_until)) return 0;
    if (!fe_own_live_visit_list(live, node->children, block_end, defer_until))
        return 0;

    if (node->kind == FE_N_LET || node->kind == FE_N_VAR ||
        node->kind == FE_N_CONST)
        return fe_own_live_add(live, node);
    return 1;
}

int fe_own_collect_last_uses(FeOwnLiveness *live, FeNode *fn)
{
    FeNode *param;
    if (!live || !fn || fn->kind != FE_N_FN) return 0;
    live->items = 0;
    live->count = 0;
    live->capacity = 0;
    live->ordinal = 0;
    for (param = fn->a ? fn->a->children : 0; param; param = param->next)
        if (!fe_own_live_add(live, param)) return 0;
    return fe_own_live_visit(live, fn->c, 0, 0);
}

const FeOwnLastUse *fe_own_last_use(const FeOwnLiveness *live,
                                    const char *cname)
{
    unsigned i;
    if (!live || !cname) return 0;
    for (i = 0; i < live->count; ++i)
        if (live->items[i].cname == cname ||
            strcmp(live->items[i].cname, cname) == 0)
            return &live->items[i];
    return 0;
}

static int fe_own_replace_unwrap(FeNode *expr)
{
    FeNode *call;
    FeNode *member;
    if (!expr || expr->kind != FE_N_MEMBER || !expr->text ||
        strcmp(expr->text,".?") != 0)
        return 0;
    call=expr->a;
    if (!call || call->kind != FE_N_CALL || !call->a ||
        call->a->kind != FE_N_MEMBER)
        return 0;
    member=call->a;
    return member->a && member->a->kind==FE_N_IDENT && member->a->text &&
           strcmp(member->a->text,"mem")==0 && member->b && member->b->text &&
           strcmp(member->b->text,"replace")==0;
}

void fe_own_mark_consumed(FeDiags *diags, int *state, FeNode *decl,
                          FeNode *expr, FeType *type, int in_defer)
{
    if (!expr || !type || fe_own_is_copy_type(type)) return;
    if (expr->kind == FE_N_INDEX && type->kind == FE_TYPE_SLICE &&
        (expr->c || !expr->b))
        return;
    if ((expr->kind == FE_N_MEMBER || expr->kind == FE_N_INDEX) &&
        !fe_own_replace_unwrap(expr)) {
        fe_diag_error(diags, expr->loc,
            "cannot move a non-Copy value out of a projection; use mem.replace");
        return;
    }
    if (expr->kind != FE_N_IDENT || !state) return;

    if (in_defer) {
        if (decl) decl->flags |= FE_OWN_NODE_DEFER_CAPTURE;
        return;
    }

    *state = FE_OWN_MOVED;
    expr->flags |= FE_OWN_NODE_CONSUMED;
}

void fe_own_check_use(FeDiags *diags, int state, FeLoc loc)
{
    if (state == FE_OWN_MOVED)
        fe_diag_error(diags, loc, "use of moved value");
    else if (state == FE_OWN_MAYBE_MOVED)
        fe_diag_error(diags, loc, "use of possibly moved value");
}

int fe_own_merge_move(int left, int right)
{
    if (left == FE_OWN_MOVED && right == FE_OWN_MOVED)
        return FE_OWN_MOVED;
    if (left != FE_OWN_AVAILABLE || right != FE_OWN_AVAILABLE)
        return FE_OWN_MAYBE_MOVED;
    return FE_OWN_AVAILABLE;
}

int fe_own_loop_entry(int before, int after)
{
    if (before == after) return before;
    return FE_OWN_MAYBE_MOVED;
}

int fe_own_loop_exit(int state, int after)
{
    if (after != FE_OWN_AVAILABLE) return FE_OWN_MAYBE_MOVED;
    return state;
}
