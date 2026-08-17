#include "checkpri.h"

FeNode *find_const_node(FeCheck *c, const char *name)
{
    FeNode *n;
    for (n=c->ast->root ? c->ast->root->children : 0; n; n=n->next)
        if (n->kind==FE_N_CONST && n->text && name && strcmp(n->text,name)==0)
            return n;
    return 0;
}


const char *builtin_format(FeCheckerState *s, FeNode *fmt)
{
    FeNode *decl;
    FeSym *sym;
    if (fmt && fmt->kind==FE_N_LITERAL && fmt->text && fmt->text[0]=='"')
        return fmt->text;
    if (fmt && fmt->kind==FE_N_IDENT) {
        sym=find_symbol(s->scope,fmt->text);
        decl=sym && sym->decl && sym->decl->kind==FE_N_CONST ?
            sym->decl : find_const_node(s->c,fmt->text);
        if (decl && decl->b && decl->b->kind==FE_N_LITERAL &&
            decl->b->text && decl->b->text[0]=='"') {
            if (!decl->a || format_is_slice_u8(fe_type_from_ast(&s->c->types,decl->a)))
                return decl->b->text;
        }
    }
    return 0;
}

int format_is_slice_u8(FeType *t)
{
    return t && t->kind==FE_TYPE_SLICE && t->elem &&
           t->elem->kind==FE_TYPE_INT && strcmp(t->elem->name,"u8")==0;
}

int format_is_writer_type(FeType *t)
{
    return t && t->kind==FE_TYPE_STRUCT &&
        (strcmp(t->name,"Writer")==0 || strcmp(t->name,"io.Writer")==0);
}

int format_arg_ok(FeType *t, int verb)
{
    if (!t) return 0;
    if (verb=='x') return fe_type_is_integer(t);
    if (verb=='c') return t->kind==FE_TYPE_CHAR;
    if (verb=='s') return format_is_slice_u8(t);
    if (verb=='b') return t->kind==FE_TYPE_BOOL;
    if (t->kind==FE_TYPE_INT || t->kind==FE_TYPE_BOOL ||
        t->kind==FE_TYPE_CHAR) return 1;
    return format_is_slice_u8(t) ||
        (t->kind==FE_TYPE_ENUM && t->is_error);
}

void check_format_call(FeCheckerState *s, FeNode *n)
{
    const char *fmt;
    FeNode *fmt_node;
    FeNode *arg;
    FeNode *x;
    FeType *t;
    unsigned long i,j;
    unsigned count=0;
    unsigned argc=0;
    unsigned offset=0;
    int verb;
    int bad=0;
    int counted=0;
    if (strcmp(n->text,"@fprint")==0) offset=1;
    fmt_node=n->children;
    if (offset) {
        if (!fmt_node) { err(s->c,n->loc,"@fprint requires a writer"); return; }
        t=check_expr(s,fmt_node);
        if (!format_is_writer_type(t))
            err(s->c,fmt_node->loc,"@fprint requires io.Writer");
        fmt_node=fmt_node->next;
    }
    if (strcmp(n->text,"@sprint")==0) {
        if (!fmt_node) { err(s->c,n->loc,"@sprint requires a buffer"); return; }
        t=check_expr(s,fmt_node);
        if (!format_is_slice_u8(t) || !t->ref_mut)
            err(s->c,fmt_node->loc,"@sprint requires []mut u8 buffer");
        fmt_node=fmt_node->next;
    }
    fmt=builtin_format(s,fmt_node);
    if (!fmt) { err(s->c,n->loc,"format must be a comptime string"); return; }
    n->aux_text=(char *)fmt;
    arg=fmt_node ? fmt_node->next : 0;
    for (x=arg;x;x=x->next) { check_expr(s,x); ++argc; }
    i=1;
    while (fmt[i] && fmt[i]!='"') {
        if (fmt[i]=='\\') { if (fmt[i+1]) ++i; ++i; continue; }
        if (fmt[i]=='{' && fmt[i+1]=='{') { i+=2; continue; }
        if (fmt[i]=='}' && fmt[i+1]=='}') { i+=2; continue; }
        if (fmt[i]=='{') {
            j=i+1;
            while (fmt[j] && fmt[j]!='}') ++j;
            if (!fmt[j]) { err(s->c,n->loc,"unterminated format placeholder"); bad=1; break; }
            if (j==i+1) verb=' '; else if (j==i+2) verb=(unsigned char)fmt[i+1]; else verb='?';
            if (verb!=' ' && verb!='x' && verb!='c' && verb!='s' && verb!='b') {
                err(s->c,n->loc,"unsupported format verb"); bad=1;
            }
            if (!arg) {
                err(s->c,n->loc,"format argument count mismatch");
                bad=1; counted=1;
            }
            else {
                t=arg->sem_type;
                if (verb==' ' && t && t->kind==FE_TYPE_ENUM && t->is_error) verb='s';
                if (!format_arg_ok(t,verb)) { err(s->c,arg->loc,"no fmt writer for argument type"); bad=1; }
                arg=arg->next;
            }
            ++count; i=j+1; continue;
        }
        if (fmt[i]=='}') { err(s->c,n->loc,"unmatched '}' in format"); bad=1; }
        ++i;
    }
    /* Running out of arguments mid-string already said this. Saying it again
       once the whole string has been walked adds nothing. */
    if (count!=argc && !counted) { err(s->c,n->loc,"format argument count mismatch"); bad=1; }
    (void)bad;
}

int is_format_builtin(const char *name)
{
    return name && (strcmp(name,"@print")==0 || strcmp(name,"@fprint")==0 ||
                    strcmp(name,"@sprint")==0);
}

int lvalue_writable(FeCheckerState *s, FeNode *n)
{
    FeSym *sym;
    FeType *t;
    if (!n) return 0;
    if (n->kind == FE_N_IDENT) {
        sym=find_symbol(s->scope,n->text ? n->text : "");
        return sym ? sym->mutable : 0;
    }
    if (n->kind == FE_N_MEMBER) {
        t=n->a ? n->a->sem_type : 0;
        if (t && t->kind==FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) return t->ref_mut;
        /* Through an owner, what may be written is decided by what is owned,
           not by whether the binding may be pointed somewhere else. `let p:
           ^[]mut T` fixes p and leaves what it owns writable. */
        if (t && t->kind==FE_TYPE_OWNED && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0)
            return !t->elem || t->elem->kind!=FE_TYPE_SLICE || t->elem->ref_mut;
        return lvalue_writable(s,n->a);
    }
    if (n->kind == FE_N_INDEX) {
        /* An index into a slice asks the slice, not the binding. */
        t=n->a ? n->a->sem_type : 0;
        if (t && t->kind==FE_TYPE_SLICE) return t->ref_mut;
        return lvalue_writable(s,n->a);
    }
    return 0;
}

int has_field(FeNode *list, const char *name)
{
    FeNode *f;
    for (f=list; f; f=f->next)
        if (f->text && name && strcmp(f->text,name)==0) return 1;
    return 0;
}

/* A field of a type declared elsewhere is reachable only with `pub`. Inside
   the declaring unit every field is reachable, `pub` or not. */
int field_is_visible(FeCheckerState *s, const FeType *t,
                            const FeFieldType *field)
{
    if (!t || !t->unit) return 1;
    if (s->c->types.unit_name &&
        strcmp(t->unit,s->c->types.unit_name)==0) return 1;
    return field && field->ast_node &&
           (field->ast_node->flags & FE_NODE_PUB)!=0;
}

/* The field list of a struct literal, once the type is known. Reached from
   both `Type{...}` and `binding.Type{...}`. */
FeType *check_struct_fields(FeCheckerState *s, FeNode *n, FeType *t)
{
    FeFieldType *field;
    FeNode *f;
    FeType *v;
    unsigned i;
    for(f=n->children;f;f=f->next) if(f->kind==FE_N_FIELD) {
        if(has_field(f->next,f->text)) { err(s->c,f->loc,"duplicate struct field"); }
        field=fe_type_field(t,f->text);
        if(!field) { err(s->c,f->loc,"invalid struct field"); continue; }
        if(!field_is_visible(s,t,field)) {
            err(s->c,f->loc,"field is private to its unit");
            continue;
        }
        v=check_expr(s,f->a);
        mark_moved(s,f->a,v);
        if(!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"struct field type mismatch");
    }
    for(i=0;i<t->field_count;i++) if(!has_field(n->children,t->fields[i].name)) err(s->c,n->loc,"missing struct field");
    n->sem_type=t; return t;
}

FeType *check_struct_init(FeCheckerState *s, FeNode *n)
{
    FeType *t;
    FeFieldType *field;
    FeNode *f;
    FeType *v;
    FeType *et;
    FeVariantType *variant;
    if (n->a && n->a->kind == FE_N_MEMBER) {
        FeUnit *home=binding_unit(s,n->a->a);
        if (home) {
            /* `binding.Type{...}` names a type in another unit. */
            const char *want=n->a->b && n->a->b->text ? n->a->b->text : "";
            FeNode *decl=unit_type_decl(s->c,home,want);
            t=unit_type(s->c,home,want);
            if (!t || !decl) { err(s->c,n->a->loc,"unknown name"); return unknown(s->c); }
            if (!decl_is_public(decl)) {
                err(s->c,n->a->loc,"type is private to its unit");
                return unknown(s->c);
            }
            if (t->kind!=FE_TYPE_STRUCT) {
                err(s->c,n->loc,"unknown struct type");
                return unknown(s->c);
            }
            return check_struct_fields(s,n,t);
        }
        et=check_expr(s,n->a->a);
        variant=et && et->kind==FE_TYPE_ENUM ?
            fe_type_variant(et,n->a->b ? n->a->b->text : "") : 0;
        if (!variant) { err(s->c,n->loc,"invalid enum variant"); return unknown(s->c); }
        if (variant->field_count != 0) {
            for (f=n->children; f; f=f->next) {
                if (f->kind != FE_N_FIELD) continue;
                field=0;
                if (variant->fields) {
                    unsigned i;
                    for(i=0;i<variant->field_count;i++) if(strcmp(variant->fields[i].name,f->text)==0) field=&variant->fields[i];
                }
                if (!field) { err(s->c,f->loc,"invalid enum payload field"); continue; }
                v=check_expr(s,f->a);
                mark_moved(s,f->a,v);
                if (!compatible(field->type,v,f->a) && v->kind!=FE_TYPE_UNKNOWN) err(s->c,f->loc,"enum payload type mismatch");
            }
        } else if (n->children) err(s->c,n->loc,"empty enum variant cannot have payload");
        n->sem_type=et; return et;
    }
    t=fe_type_intern(&s->c->types,n->text ? n->text : "<unknown>");
    if (!t || t->kind!=FE_TYPE_STRUCT) { err(s->c,n->loc,"unknown struct type"); return unknown(s->c); }
    return check_struct_fields(s,n,t);
}

FeType *check_array_init(FeCheckerState *s, FeNode *n)
{
    FeNode *x; FeType *elem=0; FeType *v; unsigned long count=0;
    for(x=n->children;x;x=x->next) { v=check_expr(s,x); mark_moved(s,x,v); if(!elem) elem=v; else if(!compatible(elem,v,x)&&v->kind!=FE_TYPE_UNKNOWN) err(s->c,x->loc,"array element type mismatch"); ++count; }
    if(!elem) elem=unknown(s->c);
    n->sem_type=fe_type_array(&s->c->types,count,elem); return n->sem_type;
}

int array_slice_lvalue(FeNode *n)
{
    return n && (n->kind==FE_N_IDENT || n->kind==FE_N_MEMBER ||
                 n->kind==FE_N_INDEX);
}

FeType *check_index(FeCheckerState *s, FeNode *n)
{
    FeType *base=check_expr(s,n->a); FeType *idx; FeType *elem;
    if(!fe_type_is_indexable(base)) { err(s->c,n->loc,"indexing requires an array or slice"); return unknown(s->c); }
    if(n->b) { idx=check_expr(s,n->b); if(known(idx)&&!fe_type_is_integer(idx)) err(s->c,n->loc,"index must be an integer"); }
    if(n->c || !n->b) {
        if (base->kind==FE_TYPE_ARRAY && !array_slice_lvalue(n->a))
            err(s->c,n->loc,"array slicing requires a stable lvalue");
        if(n->c) {
            idx=check_expr(s,n->c);
            if(known(idx)&&!fe_type_is_integer(idx))
                err(s->c,n->loc,"slice bound must be an integer");
        }
        elem=base->elem;
        n->sem_type=(base->kind==FE_TYPE_SLICE ? base->ref_mut :
                     lvalue_writable(s,n->a)) ?
            fe_type_mut_slice(&s->c->types,elem) :
            fe_type_slice(&s->c->types,elem);
        return n->sem_type;
    }
    n->sem_type=base->elem; return n->sem_type;
}

FeType *check_identifier(FeCheckerState *s, FeNode *n)
{
    FeSym *sym;
    sym = find_symbol(s->scope, n->text ? n->text : "");
    if (!sym) {
        FeType *named=fe_type_intern(&s->c->types,n->text ? n->text : "");
        if(named->kind==FE_TYPE_STRUCT || named->kind==FE_TYPE_ENUM) { n->sem_type=named; return named; }
        if(named->kind!=FE_TYPE_UNKNOWN) {
            err(s->c, n->loc, "a type is not a value here");
            return unknown(s->c);
        }
        err(s->c, n->loc, "unknown name");
        return unknown(s->c);
    }
    n->cname = sym->cname;
    n->sem_type = sym->type;
    if (!sym->fn) {
        fe_own_access(s->c->diags,&sym->own,FE_OWN_READ,n->loc);
        sym->moved=sym->own.move;
    }
    return sym->type;
}

FeType *check_expr_core(FeCheckerState *s, FeNode *n)
{
    FeCheck *c = s->c;
    FeType *a;
    FeType *b;
    FeSym *sym;
    FeNode *x;
    FeNode *param;
    FeNode *arg;
    FeType *et;
    FeFieldType *field;
    FeVariantType *variant;
    const char *op;
    if (!n) return unknown(c);
    if (n->kind == FE_N_IDENT)
        return check_identifier(s, n);
    if (n->kind == FE_N_LITERAL) {
        if (!n->text) return unknown(c);
        if (strcmp(n->text, "true") == 0 || strcmp(n->text, "false") == 0)
            a = fe_type_intern(&c->types, "bool");
        else if (n->text[0] == '\'')
            a = fe_type_intern(&c->types, "char");
        else if (n->text[0] == '"')
            a = fe_type_intern(&c->types, "str");
        else
            a = fe_type_intern(&c->types, "i32");
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_STRUCT_INIT) return check_struct_init(s,n);
    if (n->kind == FE_N_ARRAY_INIT) return check_array_init(s,n);
    if (n->kind == FE_N_INDEX) return check_index(s,n);
    if (n->kind == FE_N_MATCH) { check_match(s,n); n->sem_type=unknown(c); return n->sem_type; }
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
        } else if (strcmp(op, "try") == 0) {
            /* SPEC 6.4: try is only allowed inside a function returning an error
               union.  Checked on the expression rather than on the statement so
               that it also covers `var x = try e;` and `x = try e;`, which the
               statement-level check walked straight past. */
            if (!s->ret || s->ret->kind != FE_TYPE_ERROR_UNION)
                err(c,n->loc,"try requires an enclosing error result");
            if (a && a->kind==FE_TYPE_ERROR_UNION)
                a=a->error_value;
            else {
                err(c,n->loc,"try requires an error result");
                a=unknown(c);
            }
        } else if (strcmp(op,"&")==0 || strcmp(op,"&mut")==0) {
            if (strcmp(op,"&mut")==0 && a && a->kind==FE_TYPE_REF && !a->ref_mut)
                err(c,n->loc,"cannot create mutable borrow from a shared reference");
            own_borrow_expr(s,n->a,strcmp(op,"&mut")==0);
            a=fe_type_ref(&c->types,a,strcmp(op,"&mut")==0);
        }
        n->sem_type = a;
        return a;
    }
    if (n->kind == FE_N_TYPE && n->text && strcmp(n->text, "as") == 0) {
        a = check_expr(s, n->a);
        b = node_type(c, n->b);
        if (b->kind == FE_TYPE_VOID)
            err(c, n->loc, "cast target cannot be void");
        else if (known(a) && known(b) && !explicit_castable(a,b))
            err(c, n->loc, "'as' requires integer or char types");
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
            else if (strcmp(op,"==")!=0 && strcmp(op,"!=")!=0 &&
                     ((known(a) && !ordered_type(a)) ||
                      (known(b) && !ordered_type(b))))
                err(c, n->loc, "ordering requires integer or char operands");
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
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->b && n->a->b->text &&
            strcmp(n->a->b->text,"drop")==0) {
            err(c,n->loc,"drop may only be invoked by scope cleanup");
            return unknown(c);
        }
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
            n->a->a->kind==FE_N_IDENT && n->a->a->text &&
            strcmp(n->a->a->text,"mem")==0 && n->a->b && n->a->b->text) {
            FeNode *arg=n->children;
            if (strcmp(n->a->b->text,"destroy")==0) {
                a=arg ? check_expr(s,arg) : unknown(c);
                if (!arg || arg->next || !a || a->kind!=FE_TYPE_OWNED)
                    err(c,n->loc,"mem.destroy requires exactly one owned pointer");
                else
                    mark_moved(s,arg,a);
                n->sem_type=fe_type_intern(&c->types,"void");
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"create")==0) {
                if (!arg || arg->next)
                    err(c,n->loc,"mem.create requires exactly one value");
                a=arg ? check_expr(s,arg) : unknown(c);
                if(arg) mark_moved(s,arg,a);
                a=fe_type_owned(&c->types,a);
                n->sem_type=fe_type_error_union(&c->types,a);
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"alloc_slice")==0) {
                FeNode *count=arg ? arg->next : 0;
                FeType *item;
                {
                    /* The element type may be an instance -- `Slot(V)` -- and
                       not just a name. */
                    int named=0;
                    item=arg ? type_from_expr(s,arg,&named) : unknown(c);
                    if(!arg || !named || !count || count->next) {
                        err(c,n->loc,"mem.alloc_slice requires a type and length");
                        item=unknown(c);
                    }
                }
                b=count ? check_expr(s,count) : unknown(c);
                if(known(b) && !fe_type_is_integer(b))
                    err(c,count->loc,"slice length must be an integer");
                /* Freshly allocated storage is owned outright, so it is
                   writable: there is nobody else to disturb. */
                a=fe_type_owned(&c->types,fe_type_mut_slice(&c->types,item));
                n->sem_type=fe_type_error_union(&c->types,a);
                return n->sem_type;
            }
            if (strcmp(n->a->b->text,"replace")==0) {
                FeNode *value=arg ? arg->next : 0;
                if(!arg || !value || value->next)
                    err(c,n->loc,"mem.replace requires destination and value");
                a=arg ? check_expr(s,arg) : unknown(c);
                if(!a || a->kind!=FE_TYPE_REF || !a->ref_mut ||
                   !arg->a || !lvalue_writable(s,arg->a))
                    err(c,n->loc,"mem.replace destination must be a mutable place");
                b=value ? check_expr(s,value) : unknown(c);
                if(a && a->kind==FE_TYPE_REF && !compatible(a->elem,b,value))
                    err(c,value->loc,"mem.replace value type mismatch");
                if(value) mark_moved(s,value,b);
                n->sem_type=a && a->kind==FE_TYPE_REF ? a->elem : unknown(c);
                fe_type_require_replace(&c->types,n->sem_type);
                return n->sem_type;
            }
        }
        if (n->a && n->a->kind==FE_N_MEMBER && n->a->a &&
            n->a->a->kind==FE_N_IDENT && n->a->a->text &&
            strcmp(n->a->a->text,"io")==0 && n->a->b && n->a->b->text &&
            strcmp(n->a->b->text,"null_writer")==0) {
            FeNode *arg=n->children;
            if (arg) err(c,n->loc,"io.null_writer takes no arguments");
            n->sem_type=fe_type_intern(&c->types,"io.Writer");
            return n->sem_type;
        }
        if (n->text && is_format_builtin(n->text)) {
            check_format_call(s,n);
            if (strcmp(n->text,"@print")==0)
                n->sem_type=fe_type_intern(&c->types,"void");
            else if (strcmp(n->text,"@sprint")==0)
                n->sem_type=fe_type_intern(&c->types,"usize");
            else
                n->sem_type=fe_type_error_union(&c->types,fe_type_intern(&c->types,"void"));
            return n->sem_type;
        }
        if (!n->a && n->text && (strcmp(n->text,"@size_of")==0 || strcmp(n->text,"@align_of")==0)) {
            FeNode *type_arg=n->children;
            FeType *target=type_arg && type_arg->kind==FE_N_IDENT ? fe_type_intern(&c->types,type_arg->text) : unknown(c);
            if(!target || !known(target)) err(c,n->loc,"size/align requires a known type");
            n->sem_type=fe_type_intern(&c->types,"usize"); return n->sem_type;
        }
        if (!n->a && n->text && strcmp(n->text,"@ptr_cast")==0) {
            /* `@ptr_cast(T, p)`: the first argument names the type the result
               points at, the second is the address. R9 keeps it in `unsafe`. */
            FeNode *type_arg=n->children;
            FeNode *value=type_arg ? type_arg->next : 0;
            FeType *target=type_arg && type_arg->kind==FE_N_IDENT ?
                fe_type_intern(&c->types,type_arg->text) : unknown(c);
            if (!type_arg || !value || value->next)
                err(c,n->loc,"@ptr_cast requires a type and a pointer");
            if (value) check_expr(s,value);
            n->sem_type=fe_type_raw(&c->types,target);
            return n->sem_type;
        }
        if (n->a && n->a->kind == FE_N_MEMBER) {
            FeNode *method;
            FeNode *self_param;
            FeUnit *home=binding_unit(s,n->a->a);
            if (home) {
                const char *want=n->a->b && n->a->b->text ? n->a->b->text : "";
                FeSym *fsym=unit_member(c,home,want);
                if (!fsym) {
                    err(c,n->a->loc,"unknown name");
                    for (x=n->children;x;x=x->next) check_expr(s,x);
                    return unknown(c);
                }
                if (!decl_is_public(fsym->decl)) {
                    err(c,n->a->loc,"name is private to its unit");
                    for (x=n->children;x;x=x->next) check_expr(s,x);
                    return unknown(c);
                }
                if (decl_is_generic(fsym->fn))
                    return check_generic_call(s,n,fsym,home);
                return check_call_args(s,n,fsym,home->name,0);
            }
            {
                int names_type=0;
                FeType *owner_type=type_from_expr(s,n->a->a,&names_type);
                if (names_type && owner_type &&
                    owner_type->kind==FE_TYPE_STRUCT) {
                    FeNode *m=type_method(owner_type,
                                          n->a->b ? n->a->b->text : "");
                    if (!m) { err(c,n->a->loc,"unknown method"); return unknown(c); }
                    if (!method_is_static(m)) {
                        err(c,n->loc,"method requires a receiver");
                        return unknown(c);
                    }
                    return check_static_method_call(s,n,owner_type,m);
                }
            }
            et=check_expr(s,n->a->a);
            /* A method can be reached through a reference or an owner as well
               as through the value itself. */
            if (et && (et->kind==FE_TYPE_REF || et->kind==FE_TYPE_OWNED) &&
                et->elem && et->elem->kind==FE_TYPE_STRUCT &&
                find_method(c,et->elem,n->a->b ? n->a->b->text : ""))
                et=et->elem;
            method=et && et->kind==FE_TYPE_STRUCT ?
                find_method(c,et,n->a->b ? n->a->b->text : "") : 0;
            if(method) {
                FeBindSave msave;
                int bound=0;
                self_param=method->a ? method->a->children : 0;
                if(!self_param) {
                    err(c,n->loc,"method requires self parameter");
                    return unknown(c);
                }
                /* A method of a generic instance reads its signature with that
                   instance's arguments bound. */
                if (et->bind_count) {
                    push_instance_bindings(c,&msave,et);
                    bind_self(c,et);
                    bound=1;
                }
                a=method_type(c,self_param->a,et);
                if(a->kind==FE_TYPE_REF && a->ref_mut &&
                   !lvalue_writable(s,n->a->a))
                    err(c,n->loc,"mutable method requires a mutable receiver");
                if(a->kind!=FE_TYPE_REF) mark_moved(s,n->a->a,et);
                param=self_param->next;
                arg=n->children;
                while(param && arg) {
                    a=check_expr(s,arg);
                    b=method_type(c,param->a,et);
                    /* A method argument gets the same call-only weakening a
                       free function's does: an exclusive view may be handed
                       over as a shared one for the length of the call, and an
                       exclusive borrow is lent rather than given. */
                    if(!compatible(b,a,arg) &&
                       !(b && a && b->kind==FE_TYPE_SLICE &&
                         a->kind==FE_TYPE_SLICE && !b->ref_mut && a->ref_mut &&
                         fe_type_equal(b->elem,a->elem)) &&
                       !(b && a && b->kind==FE_TYPE_REF && a->kind==FE_TYPE_REF &&
                         !b->ref_mut && a->ref_mut &&
                         fe_type_equal(b->elem,a->elem)) &&
                       a->kind!=FE_TYPE_UNKNOWN)
                        err(c,arg->loc,"method argument type mismatch");
                    if(!call_reborrows(b,a) &&
                       !(b && a && b->kind==FE_TYPE_SLICE &&
                         a->kind==FE_TYPE_SLICE && !b->ref_mut && a->ref_mut))
                        mark_moved(s,arg,a);
                    param=param->next;
                    arg=arg->next;
                }
                if(param || arg) err(c,n->loc,"wrong number of method arguments");
                n->sem_decl=method;
                n->sem_type=method->b ? method_type(c,method->b,et) :
                    fe_type_intern(&c->types,"void");
                if (bound) {
                    pop_bindings(c,&msave);
                    check_instance_method(s,et,method,n->loc,n);
                }
                return n->sem_type;
            }
            if (et && (et->kind==FE_TYPE_SLICE || et->kind==FE_TYPE_STR) &&
                n->a->b && n->a->b->text &&
                strcmp(n->a->b->text,"trim")==0) {
                if (n->children) err(c,n->loc,"trim takes no arguments");
                n->sem_type=fe_type_slice(&c->types,et->elem);
                return n->sem_type;
            }
            variant=et && et->kind==FE_TYPE_ENUM ?
                fe_type_variant(et,n->a->b ? n->a->b->text : "") : 0;
            arg=n->children;
            if (!variant) { err(c,n->loc,"invalid enum variant constructor"); return unknown(c); }
            if (variant->field_count==1 && arg) {
                FeType *av=check_expr(s,arg);
                if(!compatible(variant->fields[0].type,av,arg)&&av->kind!=FE_TYPE_UNKNOWN) err(c,arg->loc,"enum payload type mismatch");
            } else if (variant->field_count != 0 || arg) err(c,n->loc,"wrong enum payload arity");
            n->sem_type=et; return et;
        }
        if (n->a && n->a->kind == FE_N_IDENT) {
            sym = find_symbol(s->scope, n->a->text ? n->a->text : "");
            if (!sym) {
                err(c, n->loc, "unknown function");
                return unknown(c);
            }
            if (decl_is_generic(sym->fn))
                return check_generic_call(s,n,sym,current_unit(c));
            return check_call_args(s, n, sym, 0, 0);
        }
        for (x = n->children; x; x = x->next) check_expr(s, x);
        return unknown(c);
    }
    if (n->kind == FE_N_MEMBER) {
        if (is_error_set_member(s,n)) {
            n->sem_type=fe_type_intern(&c->types,"core.Error");
            return n->sem_type;
        }
        if (n->a && n->a->kind==FE_N_IDENT && n->a->text &&
            strcmp(n->a->text,"io")==0 && n->b && n->b->text &&
            (strcmp(n->b->text,"stdout")==0 ||
             strcmp(n->b->text,"stderr")==0)) {
            n->sem_type=fe_type_intern(&c->types,"io.Writer");
            return n->sem_type;
        }
        a=check_expr(s,n->a);
        if (a->kind == FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=a->elem;
            return a->elem;
        }
        if(a->kind==FE_TYPE_REF && a->elem &&
           a->elem->kind==FE_TYPE_STRUCT) {
            field=fe_type_field(a->elem,n->b ? n->b->text : "");
            if(!field) { err(c,n->loc,"unknown struct field"); return unknown(c); }
            n->sem_type=field->type;
            return field->type;
        }
        if (a->kind == FE_TYPE_OWNED && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=a->elem;
            return a->elem;
        }
        if(a->kind==FE_TYPE_STRUCT) {
            field=fe_type_field(a,n->b ? n->b->text : "");
            if(!field) { err(c,n->loc,"unknown struct field"); return unknown(c); }
            n->sem_type=field->type; return field->type;
        }
        if(a->kind==FE_TYPE_ENUM) {
            if(!fe_type_variant(a,n->b ? n->b->text : "")) err(c,n->loc,"unknown enum variant");
            n->sem_type=a; return a;
        }
        if((a->kind==FE_TYPE_SLICE || a->kind==FE_TYPE_STR) && n->b &&
           strcmp(n->b->text,"n")==0) {
            n->sem_type=fe_type_intern(&c->types,"usize"); return n->sem_type;
        }
        return unknown(c);
    }
    return unknown(c);
}

/* `base_in` is the already-checked type of a member expression's base. The M7
   lvalue path looks at that base before delegating here, and checking it a
   second time reports any ownership violation on it a second time too. */
FeType *check_lvalue_core(FeCheckerState *s, FeNode *n, int read,
                                 FeType *base_in)
{
    FeSym *sym;
    FeType *base;
    FeFieldType *field;
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
        if (read) {
            fe_own_access(s->c->diags,&sym->own,FE_OWN_READ,n->loc);
            sym->moved=sym->own.move;
        }
        return sym->type;
    }
    if (n && n->kind == FE_N_MEMBER) {
        base=base_in ? base_in : check_expr(s,n->a);
        if (base && base->kind == FE_TYPE_REF && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            if (!base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            n->sem_type=base->elem;
            return base->elem;
        }
        if(base && base->kind==FE_TYPE_REF && base->elem &&
           base->elem->kind==FE_TYPE_STRUCT) {
            if(!base->ref_mut)
                err(s->c,n->loc,"cannot write through shared reference");
            field=fe_type_field(base->elem,n->b ? n->b->text : "");
            if(!field) { err(s->c,n->loc,"assignment requires a valid struct field"); return unknown(s->c); }
            n->sem_type=field->type;
            return field->type;
        }
        if (base && base->kind == FE_TYPE_OWNED && n->b && n->b->text &&
            strcmp(n->b->text,"^")==0) {
            n->sem_type=base->elem;
            return base->elem;
        }
        if (!lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        field=base && base->kind==FE_TYPE_STRUCT ? fe_type_field(base,n->b ? n->b->text : "") : 0;
        if(!field) { err(s->c,n->loc,"assignment requires a valid struct field"); return unknown(s->c); }
        n->sem_type=field->type; return field->type;
    }
    if (n && n->kind == FE_N_INDEX) {
        base=check_index(s,n);
        if (n->a && n->a->sem_type &&
            n->a->sem_type->kind == FE_TYPE_SLICE &&
            !n->a->sem_type->ref_mut)
            err(s->c,n->loc,"cannot write through shared slice");
        else if (n->a && n->a->sem_type &&
                 n->a->sem_type->kind != FE_TYPE_SLICE &&
                 !lvalue_writable(s,n->a))
            err(s->c,n->loc,"cannot assign through immutable value");
        return base;
    }
    if (n) err(s->c, n->loc, "assignment requires a variable");
    return unknown(s->c);
}

int compound_operator(const char *op)
{
    return op && strcmp(op, "=") != 0;
}
