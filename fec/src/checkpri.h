#ifndef FE_CHECKPRI_H
#define FE_CHECKPRI_H

/* The checker's own vocabulary, shared by the files it is split across.
   Nothing outside the checker includes this. */

#include "check.h"
#include "m7.h"
#include <stdlib.h>

#define FE_M7_FLOW_CAP 64U
#include "own.h"
#include <string.h>
#include <stdio.h>

typedef struct FeSym FeSym;
/* FeScope is forward declared in check.h. */

struct FeSym {
    const char *name;
    char *cname;
    FeType *type;
    FeNode *fn;
    int mutable;
    int initialized;
    int moved;
    FeNode *decl;
    /* M6 ownership is tracked at the root local/parameter.  A reference
       binding remembers that root so releasing the binding's last use can
       release the root borrow without a separate alias engine. */
    FeOwnState own;
    FeSym *borrow_root;
    int borrow_mut;
    int borrow_defer;
    FeScope *owner;
};

struct FeScope {
    FeScope *parent;
    FeSym *items;
    unsigned count;
    unsigned capacity;
};


typedef struct FeCheckerState {
    FeCheck *c;
    FeScope *scope;
    FeScope *globals;
    FeType *ret;
    unsigned loop_depth;
    unsigned defer_depth;
    FeOwnLiveness liveness;
    FeNode *fn_node;
} FeCheckerState;

/* The type bindings in force, saved across a nested instantiation. */
typedef struct FeBindSave {
    FeTypeBind params[FE_TYPE_PARAM_MAX];
    unsigned count;
} FeBindSave;

typedef struct FeFlowSlot {
    FeSym *sym;
    int moved;
    int initialized;
    int own_move;
    int own_initialized;
} FeFlowSlot;

typedef struct FeFlowBorrow {
    FeSym *root;
    int mutable;
} FeFlowBorrow;

/* How long a chain of new generic instances may get, and how long an
   instance's readable spelling may be before it falls back to serials. */
#define FE_GENERIC_DEPTH_MAX 32
#define FE_GENERIC_NAME_READABLE 200

/* Every definition in the checker, so the split files can see each other. */
FeType *unknown(FeCheck *c);
void err(FeCheck *c, FeLoc loc, const char *msg);
int ordered_type(const FeType *t);
int known(FeType *t);
int in_own_drop(FeCheckerState *s, FeNode *n);
void mark_moved(FeCheckerState *s, FeNode *n, FeType *t);
int compatible(FeType *want, FeType *got, FeNode *value);
int call_reborrows(const FeType *param, const FeType *arg);
int explicit_castable(FeType *a, FeType *b);
FeType *node_type(FeCheck *c, FeNode *n);
char *unit_cname(FeCheck *c, const char *name);
char *local_cname(FeCheck *c, const char *name);
FeScope *scope_new(FeCheckerState *s, FeScope *parent);
FeSym *find_current(FeScope *scope, const char *name);
FeSym *find_symbol(FeScope *scope, const char *name);
FeSym *add_symbol(FeCheckerState *s, FeScope *scope,
                         const char *name, FeType *type, FeNode *fn,
                         int mutable, int initialized, char *cname,
                         FeNode *decl);
void enter_unit(FeCheck *c, unsigned index);
unsigned unit_index(FeCheck *c, const FeUnit *u);
FeUnit *binding_unit(FeCheckerState *s, FeNode *base);
int decl_is_public(const FeNode *decl);
FeSym *unit_member(FeCheck *c, FeUnit *u, const char *name);
FeType *unit_type(FeCheck *c, FeUnit *u, const char *name);
int enter_declaring_unit(FeCheck *c, const char *unit_name);
FeNode *unit_type_decl(FeCheck *c, FeUnit *u, const char *name);
FeType *node_type_in(FeCheck *c, const char *unit, FeNode *node);
FeNode *find_method(FeCheck *c, FeType *owner, const char *name);
FeType *method_type(FeCheck *c, FeNode *node, FeType *owner);
unsigned flow_capture(FeScope *scope, FeFlowSlot *slots, unsigned cap);
void flow_restore(FeFlowSlot *slots, unsigned count);
void flow_merge(FeFlowSlot *base, FeFlowSlot *left, FeFlowSlot *right,
                       unsigned count);
FeSym *own_root_symbol(FeCheckerState *s, FeNode *expr);
int own_is_global(FeCheckerState *s, FeSym *sym);
void own_borrow_expr(FeCheckerState *s, FeNode *expr, int mutable);
void own_release_temporary_borrow(FeCheckerState *s, FeNode *expr);
FeSym *own_derived_call_root(FeCheckerState *s, FeNode *call);
void own_bind_derived_call(FeCheckerState *s, FeSym *binding,
                                  FeNode *value);
int own_stmt_uses(FeNode *node, const char *name);
int own_defer_uses(FeNode *node, const char *name);
int own_contains_node(FeNode *node, FeNode *needle);
void own_release_after_stmt(FeCheckerState *s, FeScope *scope,
                                   FeNode *stmt, int scope_end);
FeOwnState *flow_own_new(FeCheckerState *s, unsigned count);
void flow_own_capture(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count);
void flow_own_restore(FeFlowSlot *slots, FeOwnState *states,
                             unsigned count);
void flow_own_merge(FeFlowSlot *slots, FeOwnState *left,
                           FeOwnState *right, unsigned count);
FeFlowBorrow *flow_borrow_new(FeCheckerState *s, unsigned count);
void flow_borrow_capture(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count);
void flow_borrow_restore(FeFlowSlot *slots, FeFlowBorrow *states,
                                unsigned count);
void flow_borrow_merge(FeFlowSlot *slots, FeFlowBorrow *left,
                              FeFlowBorrow *right, unsigned count);
FeNode *find_const_node(FeCheck *c, const char *name);
const char *builtin_format(FeCheckerState *s, FeNode *fmt);
int format_is_slice_u8(FeType *t);
int format_is_writer_type(FeType *t);
int format_arg_ok(FeType *t, int verb);
void check_format_call(FeCheckerState *s, FeNode *n);
int is_format_builtin(const char *name);
int lvalue_writable(FeCheckerState *s, FeNode *n);
int has_field(FeNode *list, const char *name);
int field_is_visible(FeCheckerState *s, const FeType *t,
                            const FeFieldType *field);
FeType *check_struct_fields(FeCheckerState *s, FeNode *n, FeType *t);
FeType *check_struct_init(FeCheckerState *s, FeNode *n);
FeType *check_array_init(FeCheckerState *s, FeNode *n);
int array_slice_lvalue(FeNode *n);
FeType *check_index(FeCheckerState *s, FeNode *n);
FeType *check_identifier(FeCheckerState *s, FeNode *n);
FeType *check_expr_core(FeCheckerState *s, FeNode *n);
FeType *check_lvalue_core(FeCheckerState *s, FeNode *n, int read,
                                 FeType *base_in);
int compound_operator(const char *op);
void check_match(FeCheckerState *s, FeNode *n);
void check_for(FeCheckerState *s, FeNode *n);
void check_type_cycle(FeCheck *c, FeType *t);
void check_type_cycles(FeCheck *c);
int own_ast_reference_type(FeNode *type);
int own_ast_pointer_to_reference(FeNode *type);
void check_reference_storage(FeCheck *c, FeNode *decl);
int own_return_from_allowed_root(FeCheckerState *s, FeNode *expr);
void check_stmt_core(FeCheckerState *s, FeNode *n);
void check_fn(FeCheck *c, FeNode *fn, FeScope *globals);
void check_method(FeCheck *c, FeNode *fn, FeScope *globals,
                         FeType *owner);
int m7_actual_compatible(FeType *want, FeType *got, FeNode *value);
FeType *m7_check_expected(FeCheckerState *s, FeNode *value,
                                 FeType *expected);
FeType *m7_member_field(FeCheckerState *s, FeNode *n, FeType *base);
int m7_place_is_projection(FeNode *n);
unsigned decl_type_param_count(const FeNode *decl);
FeNode *decl_type_param(const FeNode *decl, unsigned i);
int decl_is_generic(const FeNode *decl);
void check_generic_params(FeCheck *c, FeNode *decl);
void push_bindings(FeCheck *c, FeBindSave *save, FeNode *decl,
                          FeType **args, unsigned count);
void push_instance_bindings(FeCheck *c, FeBindSave *save, FeType *t);
void bind_self(FeCheck *c, FeType *owner);
void pop_bindings(FeCheck *c, const FeBindSave *save);
void instance_key(char *out, const char *unit, const char *name,
                         FeType **args, unsigned count);
const char *instance_cname(FeCheck *c, const char *key);
int instance_known(FeCheck *c, const char *key);
int instance_record(FeCheck *c, const char *key, FeLoc loc,
                           FeNode *decl, FeUnit *home, FeType *owner);
int instance_descend(FeCheck *c, FeLoc loc);
FeUnit *current_unit(FeCheck *c);
FeType *build_struct_instance(FeCheck *c, FeUnit *home, FeNode *decl,
                                     const char *key, FeType **args,
                                     unsigned count);
FeType *instantiate_struct(FeCheck *c, FeUnit *home, const char *name,
                                  FeType **args, unsigned count, FeLoc loc);
FeType *instantiate_type_node(void *owner, const FeNode *node);
FeType *type_from_expr(FeCheckerState *s, FeNode *n, int *ok);
int comptime_condition(FeCheckerState *s, FeNode *n, int *out);
void instantiate_body(FeCheck *c, FeUnit *home, FeNode *decl,
                             FeType *owner, FeBindSave *bindings, FeLoc site);
FeType *check_generic_call(FeCheckerState *s, FeNode *n, FeSym *sym,
                                  FeUnit *home);
FeUnit *unit_named(FeCheck *c, const char *name);
FeType *check_static_method_call(FeCheckerState *s, FeNode *n,
                                        FeType *owner, FeNode *method);
void check_instance_method(FeCheckerState *s, FeType *owner,
                                  FeNode *method, FeLoc site, FeNode *call);
int const_names_type(FeCheckerState *s, FeNode *n);
FeNode *type_method(FeType *t, const char *name);
int method_is_static(const FeNode *method);
int is_error_set_member(FeCheckerState *s, FeNode *n);
FeType *cross_unit_value(FeCheckerState *s, FeNode *n, int *handled);
FeType *check_call_args(FeCheckerState *s, FeNode *n, FeSym *sym,
                               const char *home, unsigned skip);
FeType *check_call(FeCheckerState *s, FeNode *n);
void m7_capture_flow(FeCheckerState *s, FeFlowSlot *slots,
                            FeOwnState **own, FeFlowBorrow **borrow,
                            unsigned *count);
void m7_restore_flow(FeFlowSlot *slots, FeOwnState *own,
                            FeFlowBorrow *borrow, unsigned count);
void m7_merge_rhs_flow(FeCheckerState *s, FeFlowSlot *base,
                              FeOwnState *own_base,
                              FeFlowBorrow *borrow_base,
                              unsigned count, FeFlowSlot *rhs,
                              FeOwnState *own_rhs,
                              FeFlowBorrow *borrow_rhs);
int m7_stmt_definitely_exits(FeNode *n);
FeType *m7_check_lazy(FeCheckerState *s, FeNode *n,
                             FeM7LazyKind kind);
FeType *check_expr(FeCheckerState *s, FeNode *n);
FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read);
FeType *m7_pattern_binding_type(FeCheckerState *s, FeType *payload,
                                      FeNode *source, int *borrow_mut);
void m7_check_if_let(FeCheckerState *s, FeNode *n);
void m7_check_optional_match(FeCheckerState *s, FeNode *n,
                                    FeType *opt);
void m7_check_match_stmt(FeCheckerState *s, FeNode *n);
void m7_check_decl_stmt(FeCheckerState *s, FeNode *n, int mutable);
void check_stmt(FeCheckerState *s, FeNode *n);
int m7_ast_reference_storage(FeNode *type);
void m7_check_storage(FeCheck *c, FeNode *decl);
void m7_validate_error_decl(FeCheck *c, FeNode *decl);
void declare_unit(FeCheck *c);
FeScope *declare_unit_scope(FeCheck *c, FeCheckerState *s);
void check_unit_bodies(FeCheck *c, FeCheckerState *s);

#endif
