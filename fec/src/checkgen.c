#include "checkpri.h"

unsigned decl_type_param_count(const FeNode *decl)
{
    FeNode *p;
    unsigned n=0;
    if (!decl) return 0;
    if (decl->kind==FE_N_FN) {
        for (p=decl->a?decl->a->children:0;p;p=p->next)
            if (p->flags & FE_NODE_COMPTIME) ++n;
        return n;
    }
    if (decl->kind==FE_N_STRUCT || decl->kind==FE_N_ENUM)
        for (p=decl->a?decl->a->children:0;p;p=p->next) ++n;
    return n;
}

FeNode *decl_type_param(const FeNode *decl, unsigned i)
{
    FeNode *p;
    unsigned n=0;
    if (!decl) return 0;
    if (decl->kind==FE_N_FN) {
        for (p=decl->a?decl->a->children:0;p;p=p->next)
            if (p->flags & FE_NODE_COMPTIME) { if (n==i) return p; ++n; }
        return 0;
    }
    for (p=decl->a?decl->a->children:0;p;p=p->next) { if (n==i) return p; ++n; }
    return 0;
}

int decl_is_generic(const FeNode *decl)
{
    return decl_type_param_count(decl)!=0;
}

/* SPEC 9: v0.1 has comptime type parameters and no other kind. */
void check_generic_params(FeCheck *c, FeNode *decl)
{
    FeNode *p;
    if (!decl || decl->kind!=FE_N_FN) return;
    for (p=decl->a?decl->a->children:0;p;p=p->next) {
        if (!(p->flags & FE_NODE_COMPTIME)) continue;
        if (!p->a || p->a->kind!=FE_N_TYPE || !p->a->text ||
            strcmp(p->a->text,"type")!=0)
            err(c,p->loc,"a comptime parameter must be a type parameter");
    }
}

void push_bindings(FeCheck *c, FeBindSave *save, FeNode *decl,
                          FeType **args, unsigned count)
{
    unsigned i;
    save->count=c->types.param_count;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) save->params[i]=c->types.params[i];
    c->types.param_count=0;
    for (i=0;i<count && i<FE_TYPE_PARAM_MAX;++i) {
        FeNode *p=decl_type_param(decl,i);
        c->types.params[i].name=p && p->text ? p->text : "?";
        c->types.params[i].type=args[i];
        ++c->types.param_count;
    }
}

/* Restore the bindings recorded on an instance, so a method sees exactly the
   environment its type was built with. */
void push_instance_bindings(FeCheck *c, FeBindSave *save, FeType *t)
{
    unsigned i;
    save->count=c->types.param_count;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) save->params[i]=c->types.params[i];
    c->types.param_count=0;
    for (i=0;i<t->bind_count && i<FE_TYPE_PARAM_MAX;++i)
        c->types.params[c->types.param_count++]=t->binds[i];
}

void bind_self(FeCheck *c, FeType *owner)
{
    if (c->types.param_count>=FE_TYPE_PARAM_MAX) return;
    c->types.params[c->types.param_count].name="Self";
    c->types.params[c->types.param_count].type=owner;
    ++c->types.param_count;
}

void pop_bindings(FeCheck *c, const FeBindSave *save)
{
    unsigned i;
    for (i=0;i<FE_TYPE_PARAM_MAX;++i) c->types.params[i]=save->params[i];
    c->types.param_count=save->count;
}

/* `unit.Name(arg,arg)` -- the canonical identity of one instance.
   Nesting makes the readable spelling grow without bound, and a spelling that
   got cut off would make two different instances look like the same one, so
   past a length the arguments are written as serial numbers instead. Those are
   unique, so identity stays exact even where the spelling stops being
   readable. */

void instance_key(char *out, const char *unit, const char *name,
                         FeType **args, unsigned count)
{
    unsigned i;
    unsigned long n=0;
    unsigned long cap=(unsigned long)FE_GENERIC_NAME_READABLE;
    const char *p;
    char number[24];
    int readable=1;
    for (p=unit?unit:"";*p;++p) { if (n<cap) out[n++]=*p; else readable=0; }
    if (n<cap) out[n++]='.'; else readable=0;
    for (p=name?name:"?";*p;++p) { if (n<cap) out[n++]=*p; else readable=0; }
    if (n<cap) out[n++]='('; else readable=0;
    for (i=0;i<count && readable;++i) {
        if (i) { if (n<cap) out[n++]=','; else { readable=0; break; } }
        for (p=args[i] && args[i]->name[0] ? args[i]->name : "?";*p;++p) {
            if (n<cap) out[n++]=*p;
            else { readable=0; break; }
        }
    }
    if (readable && n<cap) out[n++]=')'; else readable=0;
    if (readable) { out[n]='\0'; return; }
    n=0;
    for (p=unit?unit:"";*p && n<cap;++p) out[n++]=*p;
    if (n<cap) out[n++]='.';
    for (p=name?name:"?";*p && n<cap;++p) out[n++]=*p;
    if (n<cap) out[n++]='(';
    for (i=0;i<count;++i) {
        if (i && n<cap) out[n++]=',';
        sprintf(number,"#%u",args[i] ? args[i]->serial : 0U);
        for (p=number;*p && n<cap;++p) out[n++]=*p;
    }
    if (n<cap) out[n++]=')';
    out[n]='\0';
}

/* Already built, or being built right now. Re-asking for a pending instance is
   how a recursive generic terminates, so it must not look like a new one. */
const char *instance_cname(FeCheck *c, const char *key)
{
    unsigned i;
    for (i=0;i<c->instance_count;++i)
        if (!strcmp(c->instances[i].key,key)) return c->instances[i].cname;
    return 0;
}

int instance_known(FeCheck *c, const char *key)
{
    unsigned i;
    for (i=0;i<c->instance_count;++i)
        if (strcmp(c->instances[i].key,key)==0) return 1;
    return 0;
}

int instance_record(FeCheck *c, const char *key, FeLoc loc,
                           FeNode *decl, FeUnit *home, FeType *owner)
{
    FeInstance *inst;
    unsigned i;
    if (instance_known(c,key)) return 0;
    if (c->instance_count>=FE_GENERIC_INSTANCE_MAX) {
        err(c,loc,"too many generic instances");
        return -1;
    }
    inst=&c->instances[c->instance_count];
    strcpy(inst->key,key);
    inst->decl=decl;
    inst->home=home ? home->name : 0;
    inst->owner=owner;
    inst->cname=unit_cname(c,key);
    /* The bindings in force right now are the ones this instance was built
       with, and lowering has to see exactly those again. */
    inst->bind_count=c->types.param_count;
    for (i=0;i<c->types.param_count && i<FE_TYPE_PARAM_MAX;++i)
        inst->binds[i]=c->types.params[i];
    ++c->instance_count;
    return 1;
}

/* One step further down a chain of instantiations. Chains that keep producing
   new instances are the ones that never end, so the limit counts nesting. */
int instance_descend(FeCheck *c, FeLoc loc)
{
    if (c->instance_depth>=FE_GENERIC_DEPTH_MAX) {
        err(c,loc,"generic instantiation depth exceeded");
        return 0;
    }
    ++c->instance_depth;
    return 1;
}

FeUnit *current_unit(FeCheck *c)
{
    unsigned u;
    for (u=0;u<c->build->count;++u)
        if (strcmp(c->build->units[u].name,c->types.unit_name)==0)
            return &c->build->units[u];
    return c->unit;
}

/* Build `Box(i32)`: the declaration's fields with the parameters bound, under
   a name that records which arguments made it. */
FeType *build_struct_instance(FeCheck *c, FeUnit *home, FeNode *decl,
                                     const char *key, FeType **args,
                                     unsigned count)
{
    FeBindSave save;
    FeType *t;
    FeNode *f;
    unsigned fields=0;
    unsigned i=0;
    t=fe_type_intern_unit(&c->types,home->name,key);
    if (!t || t->kind!=FE_TYPE_UNKNOWN) return t;
    t->kind=FE_TYPE_STRUCT;
    t->building=1;
    t->packed=(decl->flags & FE_NODE_PACKED)!=0;
    t->decl_node=decl;
    t->bind_count=0;
    for (i=0;i<count && i<FE_TYPE_PARAM_MAX;++i) {
        FeNode *p=decl_type_param(decl,i);
        t->binds[t->bind_count].name=p && p->text ? p->text : "?";
        t->binds[t->bind_count].type=args[i];
        ++t->bind_count;
    }
    t->cname=unit_cname(c,key);
    for (f=decl->children;f;f=f->next)
        if (f->kind==FE_N_FN && f->text && strcmp(f->text,"drop")==0)
            t->has_drop=1;
    for (f=decl->children;f;f=f->next) if (f->kind==FE_N_FIELD) ++fields;
    t->field_count=fields;
    if (fields) {
        const char *save_unit=c->types.unit_name;
        t->fields=(FeFieldType *)fe_arena_alloc(&c->arena,
                                                fields*sizeof(FeFieldType));
        if (!t->fields) { t->field_count=0; return t; }
        /* Field types are written in the unit that declared the struct, not in
           whichever unit asked for this instance. */
        c->types.unit_name=home->name;
        push_instance_bindings(c,&save,t);
        bind_self(c,t);
        i=0;
        for (f=decl->children;f;f=f->next) if (f->kind==FE_N_FIELD) {
            t->fields[i].name=f->text;
            t->fields[i].type=node_type(c,f->a);
            t->fields[i].offset=0;
            t->fields[i].ast_node=f;
            ++i;
        }
        pop_bindings(c,&save);
        c->types.unit_name=save_unit;
    }
    t->building=0;
    fe_type_layout_all(&c->types);
    /* A type that says how to let go of itself needs that method to exist for
       every instance, whether or not anyone calls it by name: scope cleanup
       will. */
    {
        FeNode *release;
        for (release=decl->children;release;release=release->next)
            if (release->kind==FE_N_FN && release->text &&
                !strcmp(release->text,"drop") && release->c) {
                FeCheckerState s;
                memset(&s,0,sizeof s);
                s.c=c;
                s.scope=c->unit_scope[unit_index(c,home)];
                s.globals=s.scope;
                check_instance_method(&s,t,release,decl->loc,0);
                break;
            }
    }
    return t;
}

FeType *instantiate_struct(FeCheck *c, FeUnit *home, const char *name,
                                  FeType **args, unsigned count, FeLoc loc)
{
    FeNode *decl=unit_type_decl(c,home,name);
    char key[FE_GENERIC_KEY_MAX];
    if (!decl || !decl_is_generic(decl)) {
        err(c,loc,"type does not take generic arguments");
        return unknown(c);
    }
    if (decl->kind!=FE_N_STRUCT) {
        err(c,loc,"only a generic struct can be instantiated");
        return unknown(c);
    }
    if (count!=decl_type_param_count(decl)) {
        err(c,loc,"wrong number of generic arguments");
        return unknown(c);
    }
    instance_key(key,home->name,name,args,count);
    if (instance_record(c,key,loc,decl,home,0)<0) return unknown(c);
    return build_struct_instance(c,home,decl,key,args,count);
}

/* `Name(args...)` written in type position. */
FeType *instantiate_type_node(void *owner, const FeNode *node)
{
    FeCheck *c=(FeCheck *)owner;
    FeUnit *home=current_unit(c);
    const char *name=node->text;
    FeNode *arg;
    FeType *args[FE_TYPE_PARAM_MAX];
    unsigned count=0;
    FeType *result;
    /* `binding.Name` names a type in another unit. The binding is not itself a
       type, so it has to be peeled off before anything is looked up. */
    if (node->a && node->a->kind==FE_N_IDENT && node->a->text && c->build &&
        c->unit) {
        FeUnit *bound=fe_build_binding(c->build,c->unit,node->text);
        if (bound) { home=bound; name=node->a->text; }
    }
    if (!node->children) {
        FeNode *decl=unit_type_decl(c,home,name ? name : "");
        if (decl && decl_is_generic(decl)) {
            /* A generic declaration is not a type until it has arguments. */
            err(c,node->loc,"generic type requires type arguments");
            return unknown(c);
        }
        if (name!=node->text) {
            FeType *there=unit_type(c,home,name);
            if (there) return there;
        }
        return fe_type_intern(&c->types,name);
    }
    if (!instance_descend(c,node->loc)) return unknown(c);
    for (arg=node->children;arg;arg=arg->next) {
        if (count<FE_TYPE_PARAM_MAX)
            args[count]=fe_type_from_ast(&c->types,arg);
        ++count;
    }
    if (count>FE_TYPE_PARAM_MAX) {
        err(c,node->loc,"wrong number of generic arguments");
        --c->instance_depth;
        return unknown(c);
    }
    result=instantiate_struct(c,home,name ? name : "",args,count,
                              node->loc);
    --c->instance_depth;
    return result;
}

/* A type written where an expression is: `i32`, `Box(i32)`. Only a comptime
   argument position accepts one. */
FeType *type_from_expr(FeCheckerState *s, FeNode *n, int *ok)
{
    FeCheck *c=s->c;
    FeType *t;
    unsigned i;
    *ok=0;
    if (!n) return unknown(c);
    if (n->kind==FE_N_IDENT && n->text) {
        for (i=0;i<c->types.param_count;++i)
            if (strcmp(c->types.params[i].name,n->text)==0) {
                *ok=1;
                return c->types.params[i].type;
            }
        if (find_symbol(s->scope,n->text)) {
            /* A const alias of a type is that type (SPEC 4.7). */
            FeSym *sym=find_symbol(s->scope,n->text);
            if (sym && sym->decl && sym->decl->kind==FE_N_CONST &&
                sym->decl->b && sym->decl->b->kind==FE_N_IDENT)
                return type_from_expr(s,sym->decl->b,ok);
            return unknown(c);
        }
        t=fe_type_intern(&c->types,n->text);
        if (t && t->kind!=FE_TYPE_UNKNOWN) { *ok=1; return t; }
        return unknown(c);
    }
    /* `binding.Name` names a type in another unit. */
    if (n->kind==FE_N_MEMBER && n->a && n->a->kind==FE_N_IDENT &&
        n->b && n->b->text) {
        FeUnit *bound=binding_unit(s,n->a);
        if (bound) {
            FeType *there=unit_type(c,bound,n->b->text);
            if (there) { *ok=1; return there; }
        }
        return unknown(c);
    }
    if (n->kind==FE_N_CALL && n->a &&
        (n->a->kind==FE_N_IDENT ||
         (n->a->kind==FE_N_MEMBER && n->a->a &&
          n->a->a->kind==FE_N_IDENT && n->a->b && n->a->b->text))) {
        FeType *args[FE_TYPE_PARAM_MAX];
        unsigned count=0;
        FeNode *arg;
        FeType *result;
        FeUnit *home=current_unit(c);
        const char *want;
        /* `Name(args)` here, `binding.Name(args)` when the declaration is in
           another unit. */
        if (n->a->kind==FE_N_MEMBER) {
            FeUnit *bound=binding_unit(s,n->a->a);
            if (!bound) return unknown(c);
            home=bound;
            want=n->a->b->text;
        } else {
            want=n->a->text;
        }
        if (!want || !unit_type_decl(c,home,want)) return unknown(c);
        if (!instance_descend(c,n->loc)) { *ok=1; return unknown(c); }
        for (arg=n->children;arg;arg=arg->next) {
            int inner=0;
            if (count<FE_TYPE_PARAM_MAX)
                args[count]=type_from_expr(s,arg,&inner);
            if (!inner) { --c->instance_depth; return unknown(c); }
            ++count;
        }
        if (count>FE_TYPE_PARAM_MAX) { --c->instance_depth; return unknown(c); }
        result=instantiate_struct(c,home,want,args,count,n->loc);
        --c->instance_depth;
        *ok=1;
        return result;
    }
    return unknown(c);
}

/* Can this initializer be worked out before the program runs?

   A global's bytes go into the image, so there is no moment at which a call in
   its initializer could happen -- the emitter had been quietly dropping the
   work and leaving zeros. Anything that is a name for a value already known is
   fine; anything that is work is not. */
int const_foldable(FeCheckerState *s, FeNode *n)
{
    FeNode *x;
    if (!n) return 1;
    switch (n->kind) {
    case FE_N_LITERAL:
        return 1;
    case FE_N_IDENT: {
        /* Another `const` is a name for a value; a `static`/`var` is storage
           that does not exist yet. */
        FeSym *sym=find_symbol(s->scope,n->text ? n->text : "");
        return sym && sym->decl && sym->decl->kind==FE_N_CONST;
    }
    case FE_N_MEMBER:
        /* `E.Variant`, `error.Name`, `unit.CONST` -- a name, not work. */
        if (n->a && n->a->kind==FE_N_IDENT) return 1;
        return const_foldable(s,n->a);
    case FE_N_UNARY:
        if (n->text && strcmp(n->text,"try")==0) return 0;
        return const_foldable(s,n->a);
    case FE_N_BINARY:
        if (n->text && (strcmp(n->text,"catch")==0 ||
                        strcmp(n->text,"orelse")==0)) return 0;
        return const_foldable(s,n->a) && const_foldable(s,n->b);
    case FE_N_TYPE:
        return const_foldable(s,n->a);
    case FE_N_EXPR:
        return const_foldable(s,n->a);
    case FE_N_STRUCT_INIT:
    case FE_N_ARRAY_INIT:
        for (x=n->children;x;x=x->next)
            if (!const_foldable(s,x->kind==FE_N_FIELD ? x->a : x)) return 0;
        return 1;
    default:
        return 0;
    }
}

/* A `comptime if` condition. Only the forms SPEC 9 allows: type equality and
   the type predicates. Anything else is not decidable here. */
int comptime_condition(FeCheckerState *s, FeNode *n, int *out)
{
    FeType *a;
    FeType *b;
    int ok=0;
    int eq;
    if (!n) return 0;
    if (n->kind==FE_N_BINARY && n->text &&
        (strcmp(n->text,"==")==0 || strcmp(n->text,"!=")==0)) {
        a=type_from_expr(s,n->a,&ok);
        if (!ok) return 0;
        b=type_from_expr(s,n->b,&ok);
        if (!ok) return 0;
        eq=fe_type_equal(a,b);
        *out=strcmp(n->text,"==")==0 ? eq : !eq;
        return 1;
    }
    if (n->kind==FE_N_CALL && n->text &&
        (strcmp(n->text,"@is_int")==0 || strcmp(n->text,"@is_ptr")==0)) {
        a=type_from_expr(s,n->children,&ok);
        if (!ok) return 0;
        *out=strcmp(n->text,"@is_int")==0 ? fe_type_is_integer(a) :
             (a && (a->kind==FE_TYPE_OWNED || a->kind==FE_TYPE_REF));
        return 1;
    }
    return 0;
}

/* Check a generic body once, in the unit that declared it and with the
   instance's arguments bound. Errors land on the operation that is wrong; the
   call site gets a note, because the call is context and not the defect. */
void instantiate_body(FeCheck *c, FeUnit *home, FeNode *decl,
                             FeType *owner, FeBindSave *bindings, FeLoc site)
{
    FeAst *save_ast=c->ast;
    FeUnit *save_unit=c->unit;
    const char *save_name=c->types.unit_name;
    unsigned before=c->diags->errors;
    (void)bindings;
    c->ast=&home->ast;
    c->unit=home;
    c->types.unit_name=home->name;
    fe_diags_source(c->diags,home->source,home->size);
    if (owner) check_method(c,decl,c->unit_scope[unit_index(c,home)],owner);
    else check_fn(c,decl,c->unit_scope[unit_index(c,home)]);
    c->ast=save_ast;
    c->unit=save_unit;
    c->types.unit_name=save_name;
    if (save_unit) fe_diags_source(c->diags,save_unit->source,save_unit->size);
    if (c->diags->errors>before)
        fe_diag_note_src(c->diags,site,"instantiated here");
}

/* A call to a generic function: read the type arguments, check the value
   arguments against the bound signature, then check the body once. */
FeType *check_generic_call(FeCheckerState *s, FeNode *n, FeSym *sym,
                                  FeUnit *home)
{
    FeCheck *c=s->c;
    FeNode *decl=sym->fn;
    unsigned want=decl_type_param_count(decl);
    FeType *args[FE_TYPE_PARAM_MAX];
    FeNode *arg=n->children;
    unsigned i;
    char key[FE_GENERIC_KEY_MAX];
    FeBindSave save;
    FeType *result;
    int fresh;
    if (want>FE_TYPE_PARAM_MAX) {
        err(c,n->loc,"too many generic parameters");
        return unknown(c);
    }
    for (i=0;i<want;++i) {
        int ok=0;
        if (!arg) {
            err(c,n->loc,"generic call requires explicit type arguments");
            return unknown(c);
        }
        args[i]=type_from_expr(s,arg,&ok);
        if (!ok) {
            err(c,arg->loc,"a comptime type argument must name a type");
            return unknown(c);
        }
        arg=arg->next;
    }
    instance_key(key,home->name,decl->text,args,want);
    push_bindings(c,&save,decl,args,want);
    result=check_call_args(s,n,sym,home->name,want);
    fresh=instance_record(c,key,n->loc,decl,home,0);
    pop_bindings(c,&save);
    /* The call goes to this instance, not to the declaration it came from. */
    if (n->a) n->a->cname=(char *)instance_cname(c,key);
    if (fresh>0) {
        if (!instance_descend(c,n->loc)) return result;
        push_bindings(c,&save,decl,args,want);
        instantiate_body(c,home,decl,0,&save,n->loc);
        pop_bindings(c,&save);
        --c->instance_depth;
    }
    return result;
}

/* `Type.method(...)` where Type is a generic instance and the method takes no
   self parameter. */
/* The unit a name belongs to, by name. */
FeUnit *unit_named(FeCheck *c, const char *name)
{
    unsigned u;
    if (!name) return 0;
    for (u=0;u<c->build->count;++u)
        if (!strcmp(c->build->units[u].name,name)) return &c->build->units[u];
    return 0;
}

FeType *check_static_method_call(FeCheckerState *s, FeNode *n,
                                        FeType *owner, FeNode *method)
{
    FeCheck *c=s->c;
    /* A method belongs to the unit that declared its type, not to whichever
       unit happens to be calling it. */
    FeUnit *home=unit_named(c,owner ? owner->unit : 0);
    FeBindSave save;
    FeType *result;
    char key[FE_GENERIC_KEY_MAX];
    FeType *self_args[1];
    int fresh;
    FeSym fake;
    if (!home) home=current_unit(c);
    self_args[0]=owner;
    instance_key(key,home->name,method->text,self_args,1);
    memset(&fake,0,sizeof fake);
    fake.name=method->text;
    fake.cname=method->cname;
    fake.fn=method;
    fake.decl=method;
    push_instance_bindings(c,&save,owner);
    bind_self(c,owner);
    result=check_call_args(s,n,&fake,home->name,0);
    fresh=instance_record(c,key,n->loc,method,home,owner);
    pop_bindings(c,&save);
    if (n->a) n->a->cname=(char *)instance_cname(c,key);
    if (fresh>0) {
        if (!instance_descend(c,n->loc)) return result;
        push_instance_bindings(c,&save,owner);
        bind_self(c,owner);
        instantiate_body(c,home,method,owner,&save,n->loc);
        pop_bindings(c,&save);
        --c->instance_depth;
    }
    return result;
}

/* The body of a method on a generic instance, checked once per instance. */
void check_instance_method(FeCheckerState *s, FeType *owner,
                                  FeNode *method, FeLoc site, FeNode *call)
{
    FeCheck *c=s->c;
    /* A method belongs to the unit that declared its type, not to whichever
       unit happens to be calling it. */
    FeUnit *home=unit_named(c,owner ? owner->unit : 0);
    FeBindSave save;
    char key[FE_GENERIC_KEY_MAX];
    FeType *self_args[1];
    self_args[0]=owner;
    if (!home) home=current_unit(c);
    instance_key(key,home->name,method->text,self_args,1);
    {
        FeBindSave probe;
        int fresh;
        push_instance_bindings(c,&probe,owner);
        bind_self(c,owner);
        fresh=instance_record(c,key,site,method,home,owner);
        pop_bindings(c,&probe);
        /* The call names this instance's copy of the method. */
        if (call && call->a) call->a->cname=(char *)instance_cname(c,key);
        if (fresh<=0) return;
    }
    if (!instance_descend(c,site)) return;
    push_instance_bindings(c,&save,owner);
    bind_self(c,owner);
    instantiate_body(c,home,method,owner,&save,site);
    pop_bindings(c,&save);
    --c->instance_depth;
}

/* SPEC 4.7: `const Word = i32;` is another spelling of a type, not a value.
   It has no initializer to check and no storage. */
int const_names_type(FeCheckerState *s, FeNode *n)
{
    FeType *t;
    if (!n->b || n->b->kind!=FE_N_IDENT || !n->b->text) return 0;
    if (n->a) return 0;
    if (find_symbol(s->globals,n->b->text)) return 0;
    t=fe_type_intern(&s->c->types,n->b->text);
    return t && t->kind!=FE_TYPE_UNKNOWN;
}

FeNode *type_method(FeType *t, const char *name)
{
    FeNode *m;
    if (!t || !t->decl_node || !name) return 0;
    for (m=t->decl_node->children;m;m=m->next)
        if (m->kind==FE_N_FN && m->text && strcmp(m->text,name)==0) return m;
    return 0;
}

int method_is_static(const FeNode *method)
{
    FeNode *first=method && method->a ? method->a->children : 0;
    return !first || !first->text || strcmp(first->text,"self")!=0;
}

/* A call to a named function. `home` is the unit the signature was written in,
   null when that is the unit being checked: parameter and return types have to
   be read where they were written or a name would mean the caller's type. */
/* `error.Name` is a member of the default error set. That set is open -- names
   are collected across the build and numbered later, not declared -- so any
   name is well formed here and the value's type is core.Error. */
