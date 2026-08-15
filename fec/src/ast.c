#include "ast.h"
#include <stdio.h>

void fe_ast_init(FeAst *a) { fe_arena_init(&a->arena, 32768); a->root=0; }
void fe_ast_destroy(FeAst *a) { fe_arena_destroy(&a->arena); a->root=0; }
FeNode *fe_node(FeAst *a, FeNodeKind k, FeLoc loc, const char *text, unsigned long len)
{
    FeNode *n=(FeNode *)fe_arena_alloc(&a->arena,sizeof(FeNode));
    if (!n) return 0;
    n->kind=k; n->loc=loc; n->text=text?fe_arena_strdup(&a->arena,text,len):0;
    n->a=n->b=n->c=n->children=n->next=0; n->cname=0; n->aux_text=0; n->aux_cname=0; n->sem_type=0; n->flags=0; return n;
}
void fe_node_add(FeNode *parent, FeNode *child)
{
    FeNode *p;
    if (!child) return;
    if (!parent->children) { parent->children=child; return; }
    p=parent->children; while(p->next) p=p->next; p->next=child;
}
static void spaces(int n, FILE *out) { while(n-->0) fputc(' ',out); }
void fe_ast_dump(const FeNode *n, int indent, FILE *out)
{
    const FeNode *c;
    if (!n) return;
    spaces(indent,out); fprintf(out,"(%s",fe_node_name(n->kind));
    if (n->text) fprintf(out," %s",n->text);
    fputc('\n',out);
    if (n->a) fe_ast_dump(n->a,indent+2,out);
    if (n->b) fe_ast_dump(n->b,indent+2,out);
    if (n->c) fe_ast_dump(n->c,indent+2,out);
    for(c=n->children;c;c=c->next) fe_ast_dump(c,indent+2,out);
    spaces(indent,out); fputc(')',out); fputc('\n',out);
}
const char *fe_node_name(FeNodeKind k)
{
    static const char *names[] = {"unit","import","fn","struct","enum","error","const","global","field","param","variant","block","let","var","expr-stmt","assign","if","while","for","match","arm","return","break","continue","defer","unsafe","asm","type","expr","binary","unary","call","index","member","literal","ident","struct-init","array-init","error"};
    if ((unsigned)k >= sizeof(names)/sizeof(names[0])) return "node";
    return names[k];
}
