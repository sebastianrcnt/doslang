#include "check.h"
#include <string.h>
#include <stdio.h>

typedef struct FeSym FeSym;
typedef struct FeScope FeScope;

struct FeSym {
    const char *name;
    char *cname;
    FeType *type;
    FeNode *fn;
    int mutable;
    int initialized;
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
} FeCheckerState;

static FeType *unknown(FeCheck *c)
{
    return fe_type_intern(&c->types, "<unknown>");
}

static void err(FeCheck *c, FeLoc loc, const char *msg)
{
    fe_diag_error(c->diags, loc, msg);
}

static int known(FeType *t)
{
    return t && t->kind != FE_TYPE_UNKNOWN && t->kind != FE_TYPE_ERROR;
}

static int compatible(FeType *want, FeType *got, FeNode *value)
{
    if (fe_type_equal(want, got)) return 1;
    if (!known(want) || !known(got)) return 1;
    return fe_type_is_integer(want) && fe_type_is_integer(got) && value &&
        value->kind == FE_N_LITERAL && value->text &&
        value->text[0] != '\'' && value->text[0] != '"';
}

static FeType *node_type(FeCheck *c, FeNode *n)
{
    FeType *t;
    if (!n) return unknown(c);
    t = fe_type_from_ast(&c->types, n);
    n->sem_type = t;
    return t;
}

static char *unit_cname(FeCheck *c, const char *name)
{
    char *u;
    char *p;
    unsigned long n;
    u = c->ast->root && c->ast->root->text ? c->ast->root->text : "unit";
    n = (unsigned long)strlen("fe_") + (unsigned long)strlen(u) +
        (unsigned long)strlen(name ? name : "name") + 2UL;
    p = (char *)fe_arena_alloc(&c->ast->arena, n);
    if (!p) return 0;
    strcpy(p, "fe_");
    strcat(p, u);
    strcat(p, "_");
    strcat(p, name ? name : "name");
    return p;
}

static char *local_cname(FeCheck *c, const char *name)
{
    char number[24];
    char *p;
    unsigned long n;
    sprintf(number, "%u", c->local_serial++);
    n = (unsigned long)strlen("fe_l_") + (unsigned long)strlen(name) +
        (unsigned long)strlen(number) + 2UL;
    p = (char *)fe_arena_alloc(&c->ast->arena, n);
    if (!p) return 0;
    strcpy(p, "fe_l_");
    strcat(p, name ? name : "local");
    strcat(p, "_");
    strcat(p, number);
    return p;
}

static FeScope *scope_new(FeCheckerState *s, FeScope *parent)
{
    FeScope *scope;
    scope = (FeScope *)fe_arena_alloc(&s->c->ast->arena, sizeof(FeScope));
    if (!scope) {
        err(s->c, s->c->ast->root->loc, "out of memory creating scope");
        return parent;
    }
    scope->parent = parent;
    scope->items = 0;
    scope->count = 0;
    scope->capacity = 0;
    return scope;
}

static FeSym *find_current(FeScope *scope, const char *name)
{
    unsigned i;
    if (!scope) return 0;
    for (i = scope->count; i > 0; --i)
        if (strcmp(scope->items[i - 1].name, name) == 0)
            return &scope->items[i - 1];
    return 0;
}

static FeSym *find_symbol(FeScope *scope, const char *name)
{
    FeSym *sym;
    while (scope) {
        sym = find_current(scope, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return 0;
}

static FeSym *add_symbol(FeCheckerState *s, FeScope *scope,
                         const char *name, FeType *type, FeNode *fn,
                         int mutable, int initialized, char *cname,
                         FeNode *decl)
{
    FeSym *items;
    unsigned capacity;
    FeSym *sym;
    if (!name) name = "<unnamed>";
    if (find_current(scope, name)) {
        err(s->c, decl ? decl->loc : s->c->ast->root->loc,
            "duplicate declaration in scope");
        return 0;
    }
    if (scope->count == scope->capacity) {
        capacity = scope->capacity ? scope->capacity * 2U : 8U;
        items = (FeSym *)fe_arena_alloc(&s->c->ast->arena,
                                        capacity * sizeof(FeSym));
        if (!items) {
            err(s->c, decl ? decl->loc : s->c->ast->root->loc,
                "out of memory growing symbol scope");
            return 0;
        }
        if (scope->items)
            memcpy(items, scope->items, scope->count * sizeof(FeSym));
        scope->items = items;
        scope->capacity = capacity;
    }
    sym = &scope->items[scope->count++];
    sym->name = name;
    sym->cname = cname;
    sym->type = type;
    sym->fn = fn;
    sym->mutable = mutable;
    sym->initialized = initialized;
    if (decl) {
        decl->cname = cname;
        decl->sem_type = type;
    }
    return sym;
}

void fe_check_init(FeCheck *c, FeAst *ast, FeDiags *diags,
                   unsigned pointer_bits)
{
    c->ast = ast;
    c->diags = diags;
    c->pointer_bits = pointer_bits;
    c->local_serial = 0;
    fe_types_init(&c->types, &ast->arena, pointer_bits);
}

static FeType *check_expr(FeCheckerState *s, FeNode *n);

static FeType *check_identifier(FeCheckerState *s, FeNode *n, int read)
{
    FeSym *sym;
    sym = find_symbol(s->scope, n->text ? n->text : "");
    if (!sym) {
        err(s->c, n->loc, "unknown name");
        return unknown(s->c);
    }
    n->cname = sym->cname;
    n->sem_type = sym->type;
    if (read && !sym->initialized && !sym->fn)
        err(s->c, n->loc, "use of uninitialized variable");
    return sym->type;
}

static FeType *check_expr(FeCheckerState *s, FeNode *n)
{
    FeCheck *c = s->c;
    FeType *a;
    FeType *b;
    FeSym *sym;
    FeNode *x;
    FeNode *param;
    FeNode *arg;
    const char *op;
    if (!n) return unknown(c);
    if (n->kind == FE_N_IDENT)
        return check_identifier(s, n, 1);
    if (n->kind == FE_N_LITERAL) {
        if (!n->text) return unknown(c);
        if (strcmp(n->text, "true") == 0 || strcmp(n->text, "false") == 0)
            a = fe_type_intern(&c->types, "bool");
        else if (n->text[0] == '\'')
            a = fe_type_intern(&c->types, "u8");
        else if (n->text[0] == '"')
            a = unknown(c);
        else
            a = fe_type_intern(&c->types, "i32");
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_UNARY) {
        a = check_expr(s, n->a);
        op = n->text ? n->text : "";
        if (strcmp(op, "not") == 0) {
            if (known(a) && a->kind != FE_TYPE_BOOL)
                err(c, n->loc, "'not' requires bool");
            a = fe_type_intern(&c->types, "bool");
        } else if (strcmp(op, "-") == 0) {
            if (known(a) && !fe_type_is_integer(a))
                err(c, n->loc, "unary '-' requires integer");
        }
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_TYPE && n->text && strcmp(n->text, "as") == 0) {
        a = check_expr(s, n->a);
        b = node_type(c, n->b);
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "cast target cannot be void");
        else if ((known(a) && !fe_type_is_integer(a)) ||
                 (known(b) && !fe_type_is_integer(b)))
            err(c, n->loc, "'as' requires integer types");
        n->sem_type = b;
        return b;
    }
    if (n->kind == FE_N_BINARY) {
        a = check_expr(s, n->a);
        b = check_expr(s, n->b);
        op = n->text ? n->text : "";
        if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
            if ((known(a) && a->kind != FE_TYPE_BOOL) ||
                (known(b) && b->kind != FE_TYPE_BOOL))
                err(c, n->loc, "logical operator requires bool operands");
            a = fe_type_intern(&c->types, "bool");
        } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                   strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
            if (known(a) && known(b) && !fe_type_equal(a, b) &&
                !compatible(a, b, n->b) && !compatible(b, a, n->a))
                err(c, n->loc, "comparison operands have different types");
            a = fe_type_intern(&c->types, "bool");
        } else {
            if ((known(a) && !fe_type_is_integer(a)) ||
                (known(b) && !fe_type_is_integer(b)) ||
                (known(a) && known(b) && !fe_type_equal(a, b) &&
                 !compatible(a, b, n->b) && !compatible(b, a, n->a)))
                err(c, n->loc,
                    "arithmetic operands must have the same integer type");
        }
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_CALL) {
        if (n->a && n->a->kind == FE_N_IDENT) {
            sym = find_symbol(s->scope, n->a->text ? n->a->text : "");
            if (!sym) {
                err(c, n->loc, "unknown function");
                return unknown(c);
            }
            n->a->cname = sym->cname;
            if (!sym->fn) {
                err(c, n->loc, "name is not a function");
                return unknown(c);
            }
            param = sym->fn->a ? sym->fn->a->children : 0;
            arg = n->children;
            while (param && arg) {
                a = check_expr(s, arg);
                b = node_type(c, param->a);
                if (!compatible(b, a, arg) && a->kind != FE_TYPE_UNKNOWN)
                    err(c, arg->loc, "argument type mismatch");
                param = param->next;
                arg = arg->next;
            }
            if (param || arg) err(c, n->loc, "wrong number of arguments");
            a = sym->fn->b ? node_type(c, sym->fn->b) :
                fe_type_intern(&c->types, "void");
            n->sem_type = a;
            return a;
        }
        for (x = n->children; x; x = x->next) check_expr(s, x);
        return unknown(c);
    }
    if (n->kind == FE_N_MEMBER) {
        check_expr(s, n->a);
        return unknown(c);
    }
    return unknown(c);
}

static FeType *check_lvalue(FeCheckerState *s, FeNode *n, int read)
{
    FeSym *sym;
    if (n && n->kind == FE_N_IDENT) {
        sym = find_symbol(s->scope, n->text ? n->text : "");
        if (!sym) {
            err(s->c, n->loc, "unknown name");
            return unknown(s->c);
        }
        if (sym->fn) {
            err(s->c, n->loc, "function is not assignable");
            return unknown(s->c);
        }
        if (!sym->mutable)
            err(s->c, n->loc, "cannot assign to immutable let");
        n->cname = sym->cname;
        n->sem_type = sym->type;
        if (read && !sym->initialized)
            err(s->c, n->loc, "use of uninitialized variable");
        return sym->type;
    }
    if (n) err(s->c, n->loc, "assignment requires a variable");
    return unknown(s->c);
}

static int compound_operator(const char *op)
{
    return op && strcmp(op, "=") != 0;
}

static void check_stmt(FeCheckerState *s, FeNode *n)
{
    FeCheck *c = s->c;
    FeScope *old;
    FeType *a;
    FeType *b;
    FeSym *sym;
    FeNode *x;
    int initialized;
    if (!n) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        old = s->scope;
        s->scope = scope_new(s, old);
        for (x = n->children; x; x = x->next) check_stmt(s, x);
        s->scope = old;
        break;
    case FE_N_LET:
    case FE_N_CONST:
        a = n->a ? node_type(c, n->a) : unknown(c);
        b = check_expr(s, n->b);
        if (!n->a) a = b;
        if (a->kind == FE_TYPE_VOID)
            err(c, n->loc, "variable cannot have void type");
        if (n->a && !compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "initializer type mismatch");
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "void expression cannot initialize a variable");
        add_symbol(s, s->scope, n->text, a, 0, 0, 1,
                   local_cname(c, n->text ? n->text : "local"), n);
        break;
    case FE_N_VAR:
        a = n->a ? node_type(c, n->a) : unknown(c);
        if (!n->b && !n->a)
            err(c, n->loc, "uninitialized var requires an explicit type");
        b = n->b ? check_expr(s, n->b) : unknown(c);
        if (!n->a && n->b) a = b;
        if (a->kind == FE_TYPE_VOID)
            err(c, n->loc, "variable cannot have void type");
        if (n->b && !compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "initializer type mismatch");
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "void expression cannot initialize a variable");
        initialized = n->b != 0;
        add_symbol(s, s->scope, n->text, a, 0, 1, initialized,
                   local_cname(c, n->text ? n->text : "local"), n);
        break;
    case FE_N_ASSIGN:
        b = check_expr(s, n->b);
        a = check_lvalue(s, n->a, compound_operator(n->text));
        if (!compatible(a, b, n->b) && b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "assignment type mismatch");
        sym = n->a && n->a->kind == FE_N_IDENT ?
            find_symbol(s->scope, n->a->text) : 0;
        if (sym && sym->mutable) sym->initialized = 1;
        break;
    case FE_N_EXPR_STMT:
        check_expr(s, n->a);
        break;
    case FE_N_IF:
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "if condition must be bool");
        check_stmt(s, n->b);
        check_stmt(s, n->c);
        break;
    case FE_N_WHILE:
        a = check_expr(s, n->a);
        if (known(a) && a->kind != FE_TYPE_BOOL)
            err(c, n->loc, "while condition must be bool");
        check_stmt(s, n->b);
        break;
    case FE_N_RETURN:
        b = n->a ? check_expr(s, n->a) : fe_type_intern(&c->types, "void");
        if (known(b) && b->kind == FE_TYPE_VOID && s->ret->kind != FE_TYPE_VOID)
            err(c, n->loc, "void expression returned from value function");
        else if (known(s->ret) && known(b) && !fe_type_equal(s->ret, b) &&
                 b->kind != FE_TYPE_UNKNOWN)
            err(c, n->loc, "return type mismatch");
        break;
    case FE_N_UNSAFE:
        check_stmt(s, n->a);
        break;
    default:
        break;
    }
}

static void check_fn(FeCheck *c, FeNode *fn, FeScope *globals)
{
    FeCheckerState s;
    FeScope *old;
    FeNode *x;
    FeType *t;
    s.c = c;
    s.globals = globals;
    s.scope = scope_new(&s, globals);
    s.ret = fn->b ? node_type(c, fn->b) : fe_type_intern(&c->types, "void");
    fn->sem_type = s.ret;
    for (x = fn->a ? fn->a->children : 0; x; x = x->next) {
        t = node_type(c, x->a);
        if (t->kind == FE_TYPE_VOID)
            err(c, x->loc, "parameter cannot have void type");
        add_symbol(&s, s.scope, x->text, t, 0, 1, 1,
                   local_cname(c, x->text ? x->text : "arg"), x);
    }
    old = s.scope;
    if (fn->c) check_stmt(&s, fn->c);
    s.scope = old;
}

int fe_check_program(FeCheck *c)
{
    FeCheckerState s;
    FeNode *n;
    FeSym *sym;
    FeType *t;
    FeType *iv;
    s.c = c;
    s.scope = scope_new(&s, 0);
    s.globals = s.scope;
    s.ret = fe_type_intern(&c->types, "void");
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            t = n->a ? node_type(c, n->a) : unknown(c);
            add_symbol(&s, s.globals, n->text, t, 0,
                       n->kind == FE_N_GLOBAL, n->b != 0,
                       unit_cname(c, n->text ? n->text : "global"), n);
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if (n->kind == FE_N_FN) {
            t = fe_type_intern(&c->types, "<fn>");
            add_symbol(&s, s.globals, n->text, t, n, 0, 1,
                       unit_cname(c, n->text ? n->text : "fn"), n);
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            sym = find_current(s.globals, n->text ? n->text : "");
            if (n->b) {
                iv = check_expr(&s, n->b);
                if (sym && sym->type->kind == FE_TYPE_UNKNOWN) {
                    sym->type = iv;
                    n->sem_type = iv;
                } else if (sym && !compatible(sym->type, iv, n->b) &&
                           iv->kind != FE_TYPE_UNKNOWN)
                    err(c, n->loc, "global initializer type mismatch");
                if (iv->kind == FE_TYPE_VOID)
                    err(c, n->loc, "void expression cannot initialize a global");
            }
        }
    }
    for (n = c->ast->root ? c->ast->root->children : 0; n; n = n->next)
        if (n->kind == FE_N_FN) check_fn(c, n, s.globals);
    return c->diags->errors == 0;
}

FeType *fe_check_expr_type(FeCheck *c, FeNode *n)
{
    FeCheckerState s;
    s.c = c;
    s.scope = scope_new(&s, 0);
    s.globals = s.scope;
    s.ret = fe_type_intern(&c->types, "void");
    return check_expr(&s, n);
}
