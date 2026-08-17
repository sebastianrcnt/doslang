#include "report.h"
#include <string.h>

/* What `--report-unsafe` and `--report-instances` print.
 *
 * Both answer a question that is easy to ask and easy to let slide: how much of
 * the program is outside what the checker can promise, and how much code the
 * generic instances are about to become. A number nobody can produce is not a
 * budget, so these are here rather than in a comment somewhere. */

typedef struct Counts {
    unsigned unsafe_blocks;
    unsigned raw_types;
    unsigned unchecked_calls;
} Counts;

/* Does this name end in `_unchecked`? Those are the deliberate holes in the
   checked surface, and they are worth counting separately from `unsafe`
   because they do not need a block around them. */
static int is_unchecked(const char *name)
{
    unsigned long n;
    unsigned long m = 10UL;      /* strlen("_unchecked") */
    if (!name) return 0;
    n = (unsigned long)strlen(name);
    if (n < m) return 0;
    return strcmp(name + (n - m), "_unchecked") == 0;
}

static void walk(const FeNode *n, Counts *c)
{
    const FeNode *x;
    if (!n) return;
    if (n->kind == FE_N_UNSAFE) ++c->unsafe_blocks;
    if (n->kind == FE_N_TYPE && n->text && strcmp(n->text, "*") == 0)
        ++c->raw_types;
    if (n->kind == FE_N_CALL) {
        const char *callee = n->text;
        if (!callee && n->a) {
            if (n->a->kind == FE_N_IDENT) callee = n->a->text;
            else if (n->a->kind == FE_N_MEMBER && n->a->b)
                callee = n->a->b->text;
        }
        if (is_unchecked(callee)) ++c->unchecked_calls;
    }
    walk(n->a, c);
    walk(n->b, c);
    walk(n->c, c);
    for (x = n->children; x; x = x->next) walk(x, c);
}

/* The standard library is where the unchecked things are supposed to live, so
   it is reported but kept out of the total a program is judged on. */
static int is_std(const char *unit)
{
    return unit && strncmp(unit, "std.", 4) == 0;
}

void fe_report_unsafe(const FeBuild *build, FILE *out)
{
    unsigned u;
    Counts total;
    Counts outside;
    total.unsafe_blocks = 0; total.raw_types = 0; total.unchecked_calls = 0;
    outside = total;
    fprintf(out, "%-20s %8s %8s %10s\n", "unit", "unsafe", "*T", "unchecked");
    for (u = 0; u < build->count; ++u) {
        const FeUnit *unit = &build->units[u];
        Counts c;
        c.unsafe_blocks = 0; c.raw_types = 0; c.unchecked_calls = 0;
        walk(unit->ast.root, &c);
        if (!c.unsafe_blocks && !c.raw_types && !c.unchecked_calls) continue;
        fprintf(out, "%-20s %8u %8u %10u\n", unit->name, c.unsafe_blocks,
                c.raw_types, c.unchecked_calls);
        total.unsafe_blocks += c.unsafe_blocks;
        total.raw_types += c.raw_types;
        total.unchecked_calls += c.unchecked_calls;
        if (!is_std(unit->name)) {
            outside.unsafe_blocks += c.unsafe_blocks;
            outside.raw_types += c.raw_types;
            outside.unchecked_calls += c.unchecked_calls;
        }
    }
    fprintf(out, "%-20s %8u %8u %10u\n", "total", total.unsafe_blocks,
            total.raw_types, total.unchecked_calls);
    fprintf(out, "%-20s %8u %8u %10u\n", "outside std", outside.unsafe_blocks,
            outside.raw_types, outside.unchecked_calls);
}

void fe_report_instances(const FeCheck *c, FILE *out)
{
    unsigned i;
    unsigned types = 0;
    unsigned methods = 0;
    unsigned long bytes = 0;
    fprintf(out, "%-52s %6s %8s\n", "instance", "kind", "size");
    for (i = 0; i < c->instance_count; ++i) {
        const FeInstance *inst = &c->instances[i];
        unsigned long size = 0;
        if (inst->owner) ++methods;
        else {
            ++types;
            /* A struct instance is code only through its methods; what it
               costs on its own is the storage one value of it takes. */
            {
                const FeType *t;
                for (t = c->types.types; t; t = t->next)
                    if (t->name[0] && !strcmp(t->name, inst->key)) {
                        size = t->size;
                        break;
                    }
            }
            bytes += size;
        }
        fprintf(out, "%-52s %6s %8lu\n", inst->key,
                inst->owner ? "method" : "type", size);
    }
    fprintf(out, "\n%u instances: %u types (%lu bytes of storage), %u methods\n",
            c->instance_count, types, bytes, methods);
}
