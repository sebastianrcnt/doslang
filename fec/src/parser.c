#include "parser.h"
#include <string.h>
#include <stdio.h>

static FeToken next(FeParser *p) { p->previous=p->current; p->current=fe_lexer_next(&p->lexer); return p->current; }
static int is(FeParser *p, FeTokKind k) { return p->current.kind==k; }
static int eat(FeParser *p, FeTokKind k) { if(is(p,k)){next(p);return 1;}return 0; }
static FeNode *toknode(FeParser *p, FeNodeKind k, FeToken t) { return fe_node(p->ast,k,t.loc,t.begin,t.length); }
static void error(FeParser *p, const char *s) { fe_diag_error(p->diags,p->current.loc,s); }
static int want(FeParser *p, FeTokKind k, const char *what)
{ if(eat(p,k)) return 1; error(p,what); return 0; }
static int is_name(FeParser *p) { return is(p,FE_TOK_IDENT)||is(p,FE_TOK_SELF)||is(p,FE_TOK_SELFTYPE); }
static FeNode *expr(FeParser *p, int minprec);
static FeNode *delimited_expr(FeParser *p);
static FeNode *type(FeParser *p);
static FeNode *statement(FeParser *p);
static FeNode *block(FeParser *p);

void fe_parser_init(FeParser *p, FeAst *ast, const char *src, unsigned long length, const char *file, FeDiags *d)
{
    p->ast=ast; p->diags=d; p->forbid_struct_literal=0; fe_lexer_init(&p->lexer,src,length,file,d);
    p->previous=p->current=fe_lexer_next(&p->lexer);
}

static void recover(FeParser *p)
{
    while(!is(p,FE_TOK_EOF) && !is(p,FE_TOK_SEMI) && !is(p,FE_TOK_RBRACE)) next(p);
    if(is(p,FE_TOK_SEMI)) next(p);
}

static FeNode *type_prefix(FeParser *p, FeTokKind k, FeToken op)
{
    FeNode *n;
    (void)k;
    n=toknode(p,FE_N_TYPE,op); n->a=type(p); return n;
}
static FeNode *type(FeParser *p)
{
    FeToken t=p->current; FeNode *n;
    if (is(p,FE_TOK_QUESTION)||is(p,FE_TOK_BANG)||is(p,FE_TOK_STAR)||is(p,FE_TOK_XOR)) {
        next(p); return type_prefix(p,t.kind,t);
    }
    if (is(p,FE_TOK_AND)) {
        next(p); n=toknode(p,FE_N_TYPE,t); if(eat(p,FE_TOK_MUT)) n->text=fe_arena_strdup(&p->ast->arena,"&mut",4); n->a=type(p); return n;
    }
    if (is(p,FE_TOK_LBRACKET)) {
        next(p); n=toknode(p,FE_N_TYPE,t);
        if(!eat(p,FE_TOK_RBRACKET)) { n->a=expr(p,0); want(p,FE_TOK_RBRACKET,"expected ']' in array type"); }
        else if(eat(p,FE_TOK_MUT)) n->text=fe_arena_strdup(&p->ast->arena,"[]mut",5);
        n->b=type(p); return n;
    }
    if (is(p,FE_TOK_FN)) {
        next(p); n=toknode(p,FE_N_TYPE,t); want(p,FE_TOK_LPAREN,"expected '(' in function type");
        while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)) { fe_node_add(n,type(p)); if(!eat(p,FE_TOK_COMMA)) break; }
        want(p,FE_TOK_RPAREN,"expected ')' in function type"); if(eat(p,FE_TOK_ARROW)) n->a=type(p); return n;
    }
    if (is_name(p) || is(p,FE_TOK_TYPE)) {
        next(p); n=toknode(p,FE_N_TYPE,t);
        if (eat(p,FE_TOK_DOT)) {
            if(is_name(p)) {
                FeToken mt=p->current;
                FeNode *m=toknode(p,FE_N_IDENT,mt);
                n->a=m;
                next(p);
            } else error(p,"expected type name after '.'");
        }
        if (eat(p,FE_TOK_BANG)) { FeNode *e=toknode(p,FE_N_TYPE,p->previous); e->a=n; e->b=type(p); return e; }
        if (eat(p,FE_TOK_LPAREN)) { while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)){fe_node_add(n,type(p));if(!eat(p,FE_TOK_COMMA))break;} want(p,FE_TOK_RPAREN,"expected ')' in generic type"); }
        return n;
    }
    error(p,"expected type"); next(p); return fe_node(p->ast,FE_N_TYPE,t.loc,"error",5);
}

static int precedence(FeTokKind k)
{
    switch(k) {
    case FE_TOK_ORELSE: case FE_TOK_CATCH:return 1;
    case FE_TOK_OR_KW:return 2; case FE_TOK_AND_KW:return 3;
    case FE_TOK_EQEQ: case FE_TOK_NE: case FE_TOK_LT: case FE_TOK_LE: case FE_TOK_GT: case FE_TOK_GE:return 4;
    case FE_TOK_OR:return 5; case FE_TOK_XOR:return 6; case FE_TOK_AND:return 7;
    case FE_TOK_SHL: case FE_TOK_SHR:return 8;
    case FE_TOK_PLUS: case FE_TOK_MINUS: case FE_TOK_PLUS_WRAP: case FE_TOK_MINUS_WRAP:return 9;
    case FE_TOK_STAR: case FE_TOK_SLASH: case FE_TOK_PERCENT: case FE_TOK_STAR_WRAP:return 10;
    default:return 0;
    }
}
static FeNode *primary(FeParser *p)
{
    FeToken t=p->current; FeNode *n;
    if (is(p,FE_TOK_LBRACKET)) {
        FeNode *a=toknode(p,FE_N_ARRAY_INIT,t); next(p);
        while(!is(p,FE_TOK_RBRACKET)&&!is(p,FE_TOK_EOF)) {
            fe_node_add(a,delimited_expr(p));
            if(!eat(p,FE_TOK_COMMA)) break;
        }
        want(p,FE_TOK_RBRACKET,"expected ']' after array literal");
        return a;
    }
    if(is(p,FE_TOK_INT)||is(p,FE_TOK_CHAR)||is(p,FE_TOK_STRING)||is(p,FE_TOK_TRUE)||is(p,FE_TOK_FALSE)||is(p,FE_TOK_NULL)||is(p,FE_TOK_UNDEFINED)) {next(p);return toknode(p,FE_N_LITERAL,t);}
    if(is_name(p) || is(p,FE_TOK_ERROR_KW)) {
        next(p); n=toknode(p,FE_N_IDENT,t);
        if(is(p,FE_TOK_LBRACE) && !p->forbid_struct_literal) {
            FeNode *s=toknode(p,FE_N_STRUCT_INIT,t); next(p);
            while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)) { FeNode *f;
                if(!is_name(p)){error(p,"expected field name");recover(p);break;} f=toknode(p,FE_N_FIELD,p->current);next(p);want(p,FE_TOK_COLON,"expected ':' after field");f->a=expr(p,0);fe_node_add(s,f);if(!eat(p,FE_TOK_COMMA))break;
            } want(p,FE_TOK_RBRACE,"expected '}' in struct literal"); return s;
        }
        return n;
    }
    if(eat(p,FE_TOK_LPAREN)) { int old=p->forbid_struct_literal; p->forbid_struct_literal=0; n=expr(p,0); p->forbid_struct_literal=old; want(p,FE_TOK_RPAREN,"expected ')'"); return n; }
    if(eat(p,FE_TOK_AT)) {
        FeToken name=p->current; if(!is_name(p)){error(p,"expected builtin name after '@'");return fe_node(p->ast,FE_N_ERROR_NODE,t.loc,"builtin",7);} next(p);
        n=toknode(p,FE_N_CALL,name); n->text=fe_arena_strdup(&p->ast->arena,name.begin-1,name.length+1);
        if(eat(p,FE_TOK_LPAREN)){while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)){fe_node_add(n,delimited_expr(p));if(!eat(p,FE_TOK_COMMA))break;}want(p,FE_TOK_RPAREN,"expected ')' after builtin");}
        return n;
    }
    error(p,"expected expression"); next(p); return fe_node(p->ast,FE_N_ERROR_NODE,t.loc,"expression",10);
}
static FeNode *postfix(FeParser *p)
{
    FeNode *n=primary(p);
    for(;;) {
        FeToken t=p->current; FeNode *m;
        if(eat(p,FE_TOK_LPAREN)) { m=toknode(p,FE_N_CALL,t); m->a=n; while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)){fe_node_add(m,delimited_expr(p));if(!eat(p,FE_TOK_COMMA))break;} want(p,FE_TOK_RPAREN,"expected ')' after call"); n=m; }
        else if(eat(p,FE_TOK_LBRACKET)) {
            m=toknode(p,FE_N_INDEX,t);m->a=n;
            if(is(p,FE_TOK_DOTDOT)) { m->b=0; m->flags|=FE_NODE_SLICE; } else m->b=delimited_expr(p);
            if(eat(p,FE_TOK_DOTDOT)) { m->flags|=FE_NODE_SLICE; if(!is(p,FE_TOK_RBRACKET)) m->c=delimited_expr(p); }
            want(p,FE_TOK_RBRACKET,"expected ']' after index");n=m;
        }
        else if(eat(p,FE_TOK_DOT)) {
            m=toknode(p,FE_N_MEMBER,t);m->a=n;
            if(is_name(p)){m->b=toknode(p,FE_N_IDENT,p->current);next(p);}
            else if(eat(p,FE_TOK_QUESTION)){m->text=fe_arena_strdup(&p->ast->arena,".?",2);}
            else if(eat(p,FE_TOK_XOR)){m->text=fe_arena_strdup(&p->ast->arena,".^",2);m->b=fe_node(p->ast,FE_N_IDENT,p->previous.loc,"^",1);}
            else error(p,"expected member name");
            n=m;
            if(is(p,FE_TOK_LBRACE) && !p->forbid_struct_literal) {
                FeNode *s=toknode(p,FE_N_STRUCT_INIT,t); s->a=n; next(p);
                while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)) { FeNode *f;
                    if(!is_name(p)){error(p,"expected variant field");recover(p);break;}
                    f=toknode(p,FE_N_FIELD,p->current);next(p);want(p,FE_TOK_COLON,"expected ':' after variant field");f->a=expr(p,0);fe_node_add(s,f);if(!eat(p,FE_TOK_COMMA))break;
                }
                want(p,FE_TOK_RBRACE,"expected '}' in variant constructor");n=s;
            }
        }
        else if(eat(p,FE_TOK_AS)) { m=toknode(p,FE_N_TYPE,t);m->a=n;m->b=type(p);n=m; }
        else break;
    }
    return n;
}
static FeNode *expr(FeParser *p, int minprec)
{
    FeToken t=p->current; FeNode *left,*n; int prec;
    if(is(p,FE_TOK_MINUS)||is(p,FE_TOK_NOT)||is(p,FE_TOK_XOR)||is(p,FE_TOK_AND)||is(p,FE_TOK_STAR)||is(p,FE_TOK_TRY)) { next(p); n=toknode(p,FE_N_UNARY,t); if(t.kind==FE_TOK_AND && eat(p,FE_TOK_MUT)) n->text=fe_arena_strdup(&p->ast->arena,"&mut",4); n->a=expr(p,11); left=n; }
    else left=postfix(p);
    for(;;) { t=p->current;prec=precedence(t.kind);if(prec<=minprec)break;next(p);n=toknode(p,FE_N_BINARY,t);n->a=left;if(t.kind==FE_TOK_CATCH && eat(p,FE_TOK_OR)){if(is_name(p))n->b=toknode(p,FE_N_IDENT,p->current),next(p);else error(p,"expected catch binding");want(p,FE_TOK_OR,"expected '|' after catch binding");n->c=block(p);}else n->b=expr(p,prec);left=n; }
    return left;
}

static FeNode *header_expr(FeParser *p)
{
    FeNode *n;
    int old=p->forbid_struct_literal;
    p->forbid_struct_literal=1;
    n=expr(p,0);
    p->forbid_struct_literal=old;
    return n;
}

static FeNode *delimited_expr(FeParser *p)
{
    FeNode *n;
    int old=p->forbid_struct_literal;
    p->forbid_struct_literal=0;
    n=expr(p,0);
    p->forbid_struct_literal=old;
    return n;
}

static FeNode *params(FeParser *p)
{
    FeNode *list=fe_node(p->ast,FE_N_BLOCK,p->current.loc,"params",6);
    want(p,FE_TOK_LPAREN,"expected '(' after function name");
    while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)) { FeToken t=p->current; FeNode *q; int ct=0;
        if(eat(p,FE_TOK_COMPTIME)) { t=p->previous; ct=1; }
        if(!is_name(p)){error(p,"expected parameter name");recover(p);break;} q=toknode(p,FE_N_PARAM,p->current);if(ct){q->flags|=FE_NODE_COMPTIME;q->loc=t.loc;}next(p);want(p,FE_TOK_COLON,"expected ':' in parameter");q->a=type(p);fe_node_add(list,q);if(!eat(p,FE_TOK_COMMA))break;
    }
    want(p,FE_TOK_RPAREN,"expected ')' after parameters"); return list;
}
static FeNode *fn_decl(FeParser *p, int pub, int external, int interrupt, int interrupt_safe)
{
    FeToken t=p->current, name; FeNode *n;
    (void)interrupt; (void)interrupt_safe;
    want(p,FE_TOK_FN,"expected 'fn'"); if(!is_name(p)){error(p,"expected function name");return fe_node(p->ast,FE_N_ERROR_NODE,t.loc,"fn",2);}
    name=p->current; n=toknode(p,FE_N_FN,t); if(pub) n->flags|=FE_NODE_PUB; if(external) n->flags|=FE_NODE_EXTERN; n->text=fe_arena_strdup(&p->ast->arena,name.begin,name.length); next(p); n->a=params(p); if(eat(p,FE_TOK_ARROW)) n->b=type(p); if(eat(p,FE_TOK_SEMI)) return n; n->c=block(p); return n;
}
static FeNode *field(FeParser *p, int pub)
{
    FeToken t=p->current; FeNode *n;
    if(!is_name(p)){error(p,"expected field name");recover(p);return 0;} next(p);n=toknode(p,FE_N_FIELD,t);if(pub)n->flags|=FE_NODE_PUB;want(p,FE_TOK_COLON,"expected ':' after field");n->a=type(p);if(!eat(p,FE_TOK_COMMA) && !is(p,FE_TOK_RBRACE)) error(p,"expected ',' after field");return n;
}
static FeNode *decl(FeParser *p)
{
    int pub=0, external=0, interrupt=0, interrupt_safe=0, shared=0, atomic=0; FeToken t=p->current; FeNode *n; FeTokKind before;
    (void)shared; (void)atomic;
    if(eat(p,FE_TOK_PUB)) pub=1;
    if(eat(p,FE_TOK_EXTERN)) { external=1; if(is(p,FE_TOK_STRING)) next(p); }
    if(eat(p,FE_TOK_INTERRUPT)) interrupt=1;
    if(eat(p,FE_TOK_INTERRUPT_SAFE)) interrupt_safe=1;
    if(!is(p,FE_TOK_PACKED)) t=p->current;
    if(is(p,FE_TOK_FN)) return fn_decl(p,pub,external,interrupt,interrupt_safe);
    if(eat(p,FE_TOK_PACKED)) t=p->previous;
    if(eat(p,FE_TOK_STRUCT)) { n=toknode(p,FE_N_STRUCT,t);if(pub)n->flags|=FE_NODE_PUB;if(t.kind==FE_TOK_PACKED)n->flags|=FE_NODE_PACKED;if(!is_name(p)){error(p,"expected struct name");return n;}next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);if(eat(p,FE_TOK_LPAREN)){n->a=fe_node(p->ast,FE_N_BLOCK,p->current.loc,"generics",8);while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)){fe_node_add(n->a,type(p));if(!eat(p,FE_TOK_COMMA))break;}want(p,FE_TOK_RPAREN,"expected ')' after generic parameters");}want(p,FE_TOK_LBRACE,"expected '{' in struct");while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)){int mpub=eat(p,FE_TOK_PUB);if(is(p,FE_TOK_FN))fe_node_add(n,fn_decl(p,mpub,0,0,0));else fe_node_add(n,field(p,mpub));}want(p,FE_TOK_RBRACE,"expected '}' after struct");return n; }
    if(eat(p,FE_TOK_ENUM)) { n=toknode(p,FE_N_ENUM,t);if(pub)n->flags|=FE_NODE_PUB;if(is_name(p)){next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);}else error(p,"expected enum name");if(eat(p,FE_TOK_LPAREN)){n->a=fe_node(p->ast,FE_N_BLOCK,p->current.loc,"generics",8);while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)){fe_node_add(n->a,type(p));if(!eat(p,FE_TOK_COMMA))break;}want(p,FE_TOK_RPAREN,"expected ')' after generic parameters");}want(p,FE_TOK_LBRACE,"expected '{' in enum");while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)){FeNode *v=toknode(p,FE_N_VARIANT,p->current);if(is_name(p))next(p);else{error(p,"expected variant name");recover(p);break;}if(eat(p,FE_TOK_LPAREN)){v->a=type(p);want(p,FE_TOK_RPAREN,"expected ')' in variant");}else if(eat(p,FE_TOK_LBRACE)){while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF))fe_node_add(v,field(p,1));want(p,FE_TOK_RBRACE,"expected '}' in variant");}fe_node_add(n,v);if(!eat(p,FE_TOK_COMMA))break;}want(p,FE_TOK_RBRACE,"expected '}' after enum");return n; }
    if(eat(p,FE_TOK_ERROR_KW)) { n=toknode(p,FE_N_ERROR_DECL,t);if(pub)n->flags|=FE_NODE_PUB;if(is_name(p)){next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);}else error(p,"expected error name");want(p,FE_TOK_LBRACE,"expected '{' in error declaration");while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)){FeNode *v=toknode(p,FE_N_VARIANT,p->current);if(is_name(p))next(p);else{error(p,"expected error member");recover(p);break;}want(p,FE_TOK_EQ,"expected '=' in error member");v->a=expr(p,0);want(p,FE_TOK_COMMA,"expected ',' in error declaration");fe_node_add(n,v);}want(p,FE_TOK_RBRACE,"expected '}' after error");return n; }
    if(eat(p,FE_TOK_SHARED)) { shared=1; if(eat(p,FE_TOK_ATOMIC)) atomic=1; if(!is(p,FE_TOK_VAR)) error(p,"expected 'var' after shared"); }
    if(is(p,FE_TOK_CONST)||is(p,FE_TOK_STATIC)||is(p,FE_TOK_VAR)) { FeTokKind kk=p->current.kind;next(p);n=toknode(p,kk==FE_TOK_CONST?FE_N_CONST:FE_N_GLOBAL,t);if(pub)n->flags|=FE_NODE_PUB;if(kk==FE_TOK_STATIC)n->flags|=FE_NODE_STATIC;if(shared)n->flags|=FE_NODE_SHARED;if(is_name(p)){next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);}else error(p,"expected declaration name");if(eat(p,FE_TOK_COLON))n->a=type(p);want(p,FE_TOK_EQ,"expected '=' in declaration");n->b=expr(p,0);want(p,FE_TOK_SEMI,"expected ';' after declaration");return n; }
    error(p,"expected declaration"); before=p->current.kind; recover(p);
    if (p->current.kind==before && p->current.kind!=FE_TOK_EOF) next(p);
    return 0;
}

static FeNode *block(FeParser *p)
{
    FeToken t=p->current; FeNode *n=toknode(p,FE_N_BLOCK,t);want(p,FE_TOK_LBRACE,"expected '{'");while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)){FeNode *s=statement(p);if(s)fe_node_add(n,s);}want(p,FE_TOK_RBRACE,"expected '}'");return n;
}
static FeNode *statement(FeParser *p)
{
    FeToken t=p->current; FeNode *n,*e;
    if(is(p,FE_TOK_LBRACE)) return block(p);
    if(eat(p,FE_TOK_LET)) { n=toknode(p,FE_N_LET,t);if(is_name(p)){next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);}else error(p,"expected variable name");if(eat(p,FE_TOK_COLON))n->a=type(p);want(p,FE_TOK_EQ,"expected '=' in let");n->b=expr(p,0);want(p,FE_TOK_SEMI,"expected ';'");return n; }
    if(eat(p,FE_TOK_VAR)) { n=toknode(p,FE_N_VAR,t);if(is_name(p)){next(p);n->text=fe_arena_strdup(&p->ast->arena,p->previous.begin,p->previous.length);}else error(p,"expected variable name");if(eat(p,FE_TOK_COLON))n->a=type(p);if(eat(p,FE_TOK_EQ))n->b=expr(p,0);want(p,FE_TOK_SEMI,"expected ';'");return n; }
    if(eat(p,FE_TOK_CONST)) { n=toknode(p,FE_N_CONST,t);if(is_name(p)){n->text=fe_arena_strdup(&p->ast->arena,p->current.begin,p->current.length);next(p);}else error(p,"expected constant name");if(eat(p,FE_TOK_COLON))n->a=type(p);want(p,FE_TOK_EQ,"expected '=' in const");n->b=expr(p,0);want(p,FE_TOK_SEMI,"expected ';'");return n; }
    if(eat(p,FE_TOK_IF)) {
        n=toknode(p,FE_N_IF,t);
        if(eat(p,FE_TOK_LET)) {
            FeToken pt=p->current;
            n->text=fe_arena_strdup(&p->ast->arena,"if let",6);
            if(is_name(p)) {
                n->aux_text=fe_arena_strdup(&p->ast->arena,p->current.begin,p->current.length);
                next(p);
            } else error(p,"expected if let pattern");
            if(eat(p,FE_TOK_LPAREN)) {
                if(is_name(p)) {
                    FeNode *binding=toknode(p,FE_N_IDENT,p->current);
                    next(p);
                    fe_node_add(n,binding);
                } else error(p,"expected if let binding");
                want(p,FE_TOK_RPAREN,"expected ')' in if let pattern");
            }
            want(p,FE_TOK_EQ,"expected '=' in if let");
            (void)pt;
        }
        n->a=header_expr(p);n->b=block(p);if(eat(p,FE_TOK_ELSE))n->c=is(p,FE_TOK_IF)?statement(p):block(p);return n;
    }
    if(eat(p,FE_TOK_COMPTIME)) { n=toknode(p,FE_N_IF,t);want(p,FE_TOK_IF,"expected 'if' after comptime");n->text=fe_arena_strdup(&p->ast->arena,"comptime if",11);n->a=header_expr(p);n->b=block(p);if(eat(p,FE_TOK_ELSE))n->c=is(p,FE_TOK_IF)?statement(p):block(p);return n; }
    if(eat(p,FE_TOK_WHILE)) {n=toknode(p,FE_N_WHILE,t);n->a=header_expr(p);n->b=block(p);return n;}
    if(eat(p,FE_TOK_FOR)) {n=toknode(p,FE_N_FOR,t);if(is_name(p)){n->text=fe_arena_strdup(&p->ast->arena,p->current.begin,p->current.length);next(p);}else error(p,"expected loop variable");if(eat(p,FE_TOK_COMMA)){if(is_name(p)){n->aux_text=fe_arena_strdup(&p->ast->arena,p->current.begin,p->current.length);next(p);}else error(p,"expected second loop variable");}want(p,FE_TOK_IN,"expected 'in' in for");n->a=header_expr(p);if(eat(p,FE_TOK_DOTDOT))n->c=header_expr(p);n->b=block(p);return n;}
    if(eat(p,FE_TOK_MATCH)) {
        int old=p->forbid_struct_literal;
        n=toknode(p,FE_N_MATCH,t); p->forbid_struct_literal=1; n->a=header_expr(p); p->forbid_struct_literal=old;
        want(p,FE_TOK_LBRACE,"expected '{' after match expression");
        while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)) {
            FeNode *arm=toknode(p,FE_N_ARM,p->current);
            FeToken pt=p->current;
            if(is_name(p)||is(p,FE_TOK_INT)||is(p,FE_TOK_CHAR)||is(p,FE_TOK_NULL)||is(p,FE_TOK_TRUE)||is(p,FE_TOK_FALSE)) {
                arm->text=fe_arena_strdup(&p->ast->arena,pt.begin,pt.length); next(p);
            } else { error(p,"expected match pattern"); recover(p); continue; }
            if(eat(p,FE_TOK_LPAREN)) {
                while(!is(p,FE_TOK_RPAREN)&&!is(p,FE_TOK_EOF)) {
                    if(is_name(p)) { fe_node_add(arm,toknode(p,FE_N_IDENT,p->current)); next(p); }
                    else { error(p,"expected pattern binding"); recover(p); break; }
                    if(!eat(p,FE_TOK_COMMA)) break;
                }
                want(p,FE_TOK_RPAREN,"expected ')' after match pattern");
            } else if(eat(p,FE_TOK_LBRACE)) {
                while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF)) {
                    if(is_name(p)) { fe_node_add(arm,toknode(p,FE_N_IDENT,p->current)); next(p); }
                    else { error(p,"expected field binding"); recover(p); break; }
                    if(!eat(p,FE_TOK_COMMA)) break;
                }
                want(p,FE_TOK_RBRACE,"expected '}' after match pattern");
            }
            want(p,FE_TOK_FATARROW,"expected '=>' in match arm");
            if(is(p,FE_TOK_LBRACE)) arm->a=block(p);
            else { arm->a=expr(p,0); want(p,FE_TOK_SEMI,"expected ';' in match arm"); }
            fe_node_add(n,arm);
        }
        want(p,FE_TOK_RBRACE,"expected '}' after match"); return n;
    }
    if(eat(p,FE_TOK_RETURN)) {n=toknode(p,FE_N_RETURN,t);if(!is(p,FE_TOK_SEMI))n->a=expr(p,0);want(p,FE_TOK_SEMI,"expected ';' after return");return n;}
    if(eat(p,FE_TOK_BREAK)){n=toknode(p,FE_N_BREAK,t);want(p,FE_TOK_SEMI,"expected ';'");return n;}
    if(eat(p,FE_TOK_CONTINUE)){n=toknode(p,FE_N_CONTINUE,t);want(p,FE_TOK_SEMI,"expected ';'");return n;}
    if(eat(p,FE_TOK_DEFER)){n=toknode(p,FE_N_DEFER,t);n->a=block(p);return n;}
    if(eat(p,FE_TOK_UNSAFE)){n=toknode(p,FE_N_UNSAFE,t);n->a=block(p);return n;}
    if(eat(p,FE_TOK_CRITICAL)){n=toknode(p,FE_N_UNSAFE,t);n->text=fe_arena_strdup(&p->ast->arena,"critical",8);n->a=block(p);return n;}
    if(eat(p,FE_TOK_ASM)){n=toknode(p,FE_N_ASM,t);want(p,FE_TOK_LBRACE,"expected '{' after asm");while(!is(p,FE_TOK_RBRACE)&&!is(p,FE_TOK_EOF))next(p);want(p,FE_TOK_RBRACE,"expected '}' after asm");return n;}
    e=expr(p,0); if(is(p,FE_TOK_EQ)||is(p,FE_TOK_PLUS_EQ)||is(p,FE_TOK_MINUS_EQ)||is(p,FE_TOK_STAR_EQ)||is(p,FE_TOK_SLASH_EQ)||is(p,FE_TOK_PERCENT_EQ)||is(p,FE_TOK_AND_EQ)||is(p,FE_TOK_OR_EQ)||is(p,FE_TOK_XOR_EQ)||is(p,FE_TOK_SHL_EQ)||is(p,FE_TOK_SHR_EQ)){n=toknode(p,FE_N_ASSIGN,p->current);n->a=e;next(p);n->b=expr(p,0);}else{n=toknode(p,FE_N_EXPR_STMT,t);n->a=e;}want(p,FE_TOK_SEMI,"expected ';' after statement");return n;
}

/* A unit path is dotted: `game.world.map`. It is stored canonically, dots and
   all, because that spelling is the unit's identity everywhere else. */
static char *unit_path(FeParser *p)
{
    char buf[256];
    unsigned long len=0;
    if(!is_name(p)) return 0;
    for(;;) {
        unsigned long n=p->current.length;
        if(len && len+1<sizeof buf) buf[len++]='.';
        if(len+n>=sizeof buf){error(p,"unit path is too long");return 0;}
        memcpy(buf+len,p->current.begin,n);
        len+=n;
        next(p);
        if(!eat(p,FE_TOK_DOT)) break;
        if(!is_name(p)){error(p,"expected a name after '.' in unit path");return 0;}
    }
    return fe_arena_strdup(&p->ast->arena,buf,len);
}

FeNode *fe_parse_unit(FeParser *p)
{
    FeToken t=p->current; FeNode *root; char *path;
    if(!eat(p,FE_TOK_UNIT)){error(p,"source must start with 'unit'");return fe_node(p->ast,FE_N_ERROR_NODE,t.loc,"unit",4);}
    root=toknode(p,FE_N_UNIT,t);
    path=unit_path(p);
    if(path) root->text=path; else error(p,"expected unit name");
    want(p,FE_TOK_SEMI,"expected ';' after unit name");
    while(eat(p,FE_TOK_IMPORT)){
        FeToken it=p->previous;FeNode *i=toknode(p,FE_N_IMPORT,it);
        path=unit_path(p);
        if(path) i->text=path; else error(p,"expected import name");
        /* `as` renames the binding; without it the binding is the last segment. */
        if(eat(p,FE_TOK_AS)) {
            if(is_name(p)){i->aux_text=fe_arena_strdup(&p->ast->arena,p->current.begin,p->current.length);next(p);}
            else error(p,"expected an alias name after 'as'");
        }
        want(p,FE_TOK_SEMI,"expected ';' after import");
        fe_node_add(root,i);
    }
    while(!is(p,FE_TOK_EOF)){FeNode *d=decl(p);if(d)fe_node_add(root,d);}
    return root;
}
