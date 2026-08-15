#include "emit_c.h"
#include <stdio.h>
#include <string.h>

static void pad(FeEmitter *e)
{
    int i;
    for (i = 0; i < e->indent; ++i) fputs("    ", e->out);
}

static const char *ctype(FeEmitter *e, FeNode *n)
{
    FeType *t;
    if (n && n->sem_type) t = n->sem_type;
    else if (n) t = fe_type_from_ast(&e->check->types, n);
    else t = fe_type_intern(&e->check->types, "i32");
    return fe_type_c_name(t, e->pointer_bits);
}

static const char *cname(FeNode *n, const char *fallback)
{
    return n && n->cname ? n->cname : fallback;
}

static void emit_expr(FeEmitter *e, FeNode *n);
static void emit_stmt(FeEmitter *e, FeNode *n);

static void emit_expr(FeEmitter *e, FeNode *n)
{
    FeNode *x;
    const char *op;
    if (!n) {
        fputs("0", e->out);
        return;
    }
    switch (n->kind) {
    case FE_N_IDENT:
        fputs(cname(n, "fe_missing"), e->out);
        break;
    case FE_N_LITERAL:
        if (n->text && strcmp(n->text, "true") == 0) fputs("1", e->out);
        else if (n->text && strcmp(n->text, "false") == 0) fputs("0", e->out);
        else fputs(n->text ? n->text : "0", e->out);
        break;
    case FE_N_UNARY:
        op = n->text ? n->text : "";
        if (strcmp(op, "not") == 0) fputs("(!", e->out);
        else {
            fputc('(', e->out);
            fputs(op, e->out);
        }
        emit_expr(e, n->a);
        fputc(')', e->out);
        break;
    case FE_N_BINARY:
        op = n->text ? n->text : "+";
        fputc('(', e->out);
        emit_expr(e, n->a);
        if (strcmp(op, "and") == 0) fputs(" && ", e->out);
        else if (strcmp(op, "or") == 0) fputs(" || ", e->out);
        else fputs(op, e->out);
        emit_expr(e, n->b);
        fputc(')', e->out);
        break;
    case FE_N_TYPE:
        if (n->text && strcmp(n->text, "as") == 0) {
            fputs("((", e->out);
            fputs(ctype(e, n->b), e->out);
            fputc(')', e->out);
            emit_expr(e, n->a);
            fputc(')', e->out);
        } else emit_expr(e, n->a);
        break;
    case FE_N_CALL:
        if (n->a) emit_expr(e, n->a);
        else fputs(n->text ? n->text : "fe_builtin", e->out);
        fputc('(', e->out);
        for (x = n->children; x; x = x->next) {
            if (x != n->children) fputs(", ", e->out);
            emit_expr(e, x);
        }
        fputc(')', e->out);
        break;
    case FE_N_MEMBER:
        emit_expr(e, n->a);
        fputc('.', e->out);
        if (n->b) fputs(n->b->text ? n->b->text : "member", e->out);
        break;
    default:
        fputs("0", e->out);
        break;
    }
}

static void emit_decl(FeEmitter *e, FeNode *n)
{
    pad(e);
    fputs(ctype(e, n), e->out);
    fputc(' ', e->out);
    fputs(cname(n, "fe_local"), e->out);
    fputs(";\n", e->out);
}

static void emit_block(FeEmitter *e, FeNode *n)
{
    FeNode *x;
    if (!n) {
        pad(e);
        fputs("{\n", e->out);
        ++e->indent;
        --e->indent;
        pad(e);
        fputc('}', e->out);
        return;
    }
    pad(e);
    fputs("{\n", e->out);
    ++e->indent;
    /* C89 requires declarations before statements in each actual block. */
    for (x = n->children; x; x = x->next)
        if (x->kind == FE_N_LET || x->kind == FE_N_VAR) emit_decl(e, x);
    for (x = n->children; x; x = x->next) emit_stmt(e, x);
    --e->indent;
    pad(e);
    fputc('}', e->out);
}

static void emit_stmt(FeEmitter *e, FeNode *n)
{
    if (!n) return;
    switch (n->kind) {
    case FE_N_BLOCK:
        emit_block(e, n);
        fputc('\n', e->out);
        break;
    case FE_N_LET:
    case FE_N_VAR:
        if (n->b) {
            pad(e);
            fputs(cname(n, "fe_local"), e->out);
            fputs(" = ", e->out);
            emit_expr(e, n->b);
            fputs(";\n", e->out);
        }
        break;
    case FE_N_ASSIGN:
        pad(e);
        emit_expr(e, n->a);
        fputc(' ', e->out);
        fputs(n->text ? n->text : "=", e->out);
        fputs(" ", e->out);
        emit_expr(e, n->b);
        fputs(";\n", e->out);
        break;
    case FE_N_EXPR_STMT:
        pad(e);
        emit_expr(e, n->a);
        fputs(";\n", e->out);
        break;
    case FE_N_RETURN:
        pad(e);
        fputs("return", e->out);
        if (n->a) {
            fputc(' ', e->out);
            emit_expr(e, n->a);
        }
        fputs(";\n", e->out);
        break;
    case FE_N_IF:
        pad(e);
        fputs("if (", e->out);
        emit_expr(e, n->a);
        fputs(") ", e->out);
        if (n->b && n->b->kind == FE_N_BLOCK) emit_block(e, n->b);
        else emit_block(e, 0);
        if (n->c) {
            fputs(" else ", e->out);
            if (n->c->kind == FE_N_IF) emit_stmt(e, n->c);
            else emit_block(e, n->c);
        }
        fputc('\n', e->out);
        break;
    case FE_N_WHILE:
        pad(e);
        fputs("while (", e->out);
        emit_expr(e, n->a);
        fputs(") ", e->out);
        if (n->b && n->b->kind == FE_N_BLOCK) emit_block(e, n->b);
        else emit_block(e, 0);
        fputc('\n', e->out);
        break;
    default:
        break;
    }
}

static void emit_fn(FeEmitter *e, FeNode *fn, int prototype)
{
    FeNode *p;
    const char *ret;
    ret = fn->sem_type ? fe_type_c_name(fn->sem_type, e->pointer_bits) :
        (fn->b ? ctype(e, fn->b) : "void");
    fputs(ret, e->out);
    fputc(' ', e->out);
    fputs(cname(fn, "fe_fn"), e->out);
    fputc('(', e->out);
    p = fn->a ? fn->a->children : 0;
    if (!p) fputs("void", e->out);
    while (p) {
        if (p != fn->a->children) fputs(", ", e->out);
        fputs(ctype(e, p->a), e->out);
        fputc(' ', e->out);
        fputs(cname(p, "fe_arg"), e->out);
        p = p->next;
    }
    fputc(')', e->out);
    if (prototype) fputs(";\n", e->out);
    else {
        fputs(" ", e->out);
        emit_block(e, fn->c);
        fputc('\n', e->out);
    }
}

static void emit_main_wrapper(FeEmitter *e, FeNode *fn)
{
    fputs("int main(void) {\n    ", e->out);
    if (fn->sem_type && fn->sem_type->kind == FE_TYPE_VOID) {
        fputs(cname(fn, "fe_main"), e->out);
        fputs("();\n    return 0;\n", e->out);
    } else {
        fputs("return ", e->out);
        fputs(cname(fn, "fe_main"), e->out);
        fputs("();\n", e->out);
    }
    fputs("}\n", e->out);
}

void fe_emit_c_init(FeEmitter *e, FILE *out, FeCheck *check,
                    unsigned pointer_bits)
{
    e->out = out;
    e->check = check;
    e->pointer_bits = pointer_bits;
    e->indent = 0;
}

void fe_emit_c_program(FeEmitter *e)
{
    FeNode *n;
    FeNode *main_fn = 0;
    fputs("/* generated by fec M2 */\n#include <stddef.h>\n\n", e->out);
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next) {
        if (n->kind == FE_N_GLOBAL || n->kind == FE_N_CONST) {
            fputs(ctype(e, n), e->out);
            fputc(' ', e->out);
            fputs(cname(n, "fe_global"), e->out);
            if (n->b) {
                fputs(" = ", e->out);
                emit_expr(e, n->b);
            }
            fputs(";\n", e->out);
        }
    }
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next)
        if (n->kind == FE_N_FN) {
            emit_fn(e, n, 1);
            if (n->text && strcmp(n->text, "main") == 0) main_fn = n;
        }
    fputc('\n', e->out);
    for (n = e->check->ast->root ? e->check->ast->root->children : 0;
         n; n = n->next)
        if (n->kind == FE_N_FN) emit_fn(e, n, 0);
    if (main_fn) {
        fputc('\n', e->out);
        emit_main_wrapper(e, main_fn);
    }
}
