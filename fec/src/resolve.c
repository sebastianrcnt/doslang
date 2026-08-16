#include "resolve.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int segment_ok(const char *s, unsigned long n, const char **why)
{
    unsigned long i;
    if (!n) { *why = "unit path segment is empty"; return 0; }
    if (n > FE_UNIT_SEGMENT_MAX) {
        *why = "unit path segment is longer than eight characters";
        return 0;
    }
    if (s[0] < 'a' || s[0] > 'z') {
        *why = "unit path segment must start with a lowercase letter";
        return 0;
    }
    for (i = 1; i < n; ++i) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') continue;
        *why = "unit path segment may only contain lowercase letters, digits and '_'";
        return 0;
    }
    return 1;
}

/* Compare a dotted unit path against the source path it was read from.
   `game.world.map` matches `.../game/world/map.fe` and nothing else. Only the
   trailing segments are compared, since the leading part is the import root. */
static int path_matches(const char *unit, const char *source)
{
    unsigned long ulen = strlen(unit), slen = strlen(source);
    unsigned long u, s;
    if (slen < 3 || strcmp(source + slen - 3, ".fe") != 0) return 0;
    slen -= 3;
    u = ulen;
    s = slen;
    while (u > 0) {
        char uc, sc;
        --u;
        if (s == 0) return 0;
        --s;
        uc = unit[u];
        sc = source[s];
        if (uc == '.') {
            if (sc != '/' && sc != '\\') return 0;
            continue;
        }
        /* Host filesystems may be case-insensitive; the unit name is the
           authority and is lowercase by 8.1, so fold the path side down. */
        if (sc >= 'A' && sc <= 'Z') sc = (char)(sc - 'A' + 'a');
        if (uc != sc) return 0;
    }
    /* What remains of the source path is the import root, and must end there. */
    return s == 0 || source[s - 1] == '/' || source[s - 1] == '\\';
}

static char *read_source(const char *path, unsigned long *size)
{
    FILE *f;
    long n;
    char *p;
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    p = (char *)malloc((unsigned long)n + 1);
    if (!p) { fclose(f); return 0; }
    if (fread(p, 1, (unsigned long)n, f) != (unsigned long)n) {
        fclose(f); free(p); return 0;
    }
    p[n] = 0;
    fclose(f);
    *size = (unsigned long)n;
    return p;
}

/* <root>/a/b.fe for the unit a.b */
static void unit_source_path(char *out, unsigned long cap,
                             const char *root, const char *unit)
{
    unsigned long i = 0, j = 0;
    while (root[i] && j + 1 < cap) out[j++] = root[i++];
    if (j && out[j - 1] != '/' && out[j - 1] != '\\' && j + 1 < cap) out[j++] = '/';
    for (i = 0; unit[i] && j + 1 < cap; ++i)
        out[j++] = unit[i] == '.' ? '/' : unit[i];
    if (j + 3 < cap) { out[j++] = '.'; out[j++] = 'f'; out[j++] = 'e'; }
    out[j] = 0;
}

/* Strip the unit's own path from the file it was read from; what is left is
   the import root that every other unit is looked up under. */
static void import_root(char *out, unsigned long cap,
                        const char *source, const char *unit)
{
    unsigned long slen = strlen(source);
    unsigned long dots = 0, i, cut;
    for (i = 0; unit[i]; ++i) if (unit[i] == '.') ++dots;
    if (slen >= 3) slen -= 3;
    cut = slen;
    for (i = 0; i <= dots; ++i) {
        while (cut > 0 && source[cut - 1] != '/' && source[cut - 1] != '\\') --cut;
        if (i < dots && cut > 0) --cut;
    }
    if (cut >= cap) cut = cap - 1;
    memcpy(out, source, cut);
    out[cut] = 0;
    if (!cut) { out[0] = '.'; out[1] = 0; }
}

static FeUnit *find_unit(FeBuild *b, const char *name)
{
    unsigned i;
    for (i = 0; i < b->count; ++i)
        if (strcmp(b->units[i].name, name) == 0) return &b->units[i];
    return 0;
}

const char *fe_import_binding(const FeNode *import)
{
    const char *dot;
    if (!import) return 0;
    if (import->aux_text) return import->aux_text;
    dot = import->text ? strrchr(import->text, '.') : 0;
    return dot ? dot + 1 : import->text;
}

int fe_resolve_unit_identity(FeAst *ast, FeDiags *diags, const char *source_path)
{
    FeNode *root = ast ? ast->root : 0;
    const char *name, *why;
    const char *seg;
    unsigned long i, len;
    int ok = 1;

    if (!root || root->kind != FE_N_UNIT || !root->text) return 0;
    name = root->text;
    len = strlen(name);

    seg = name;
    for (i = 0; i <= len; ++i) {
        if (i != len && name[i] != '.') continue;
        if (!segment_ok(seg, (unsigned long)(name + i - seg), &why)) {
            fe_diag_error(diags, root->loc, why);
            ok = 0;
        }
        seg = name + i + 1;
    }

    if (ok && source_path && !path_matches(name, source_path)) {
        fe_diag_errorf(diags, root->loc,
                       "unit %s must be declared in a source file matching its path",
                       name);
        ok = 0;
    }
    return ok;
}

/* Depth-first load. `stack` is the chain of units currently being loaded, so
   meeting one again is a cycle rather than a repeat visit. */
static int load_unit(FeBuild *b, const char *name, FeLoc from, int have_from,
                     const char **stack, unsigned depth)
{
    FeUnit *unit;
    FeNode *n;
    FeParser p;
    unsigned long size;
    unsigned i;
    int ok = 1;

    for (i = 0; i < depth; ++i) {
        if (strcmp(stack[i], name) == 0) {
            fe_diag_errorf(b->diags, from, "import of %s forms a cycle", name);
            return 0;
        }
    }
    if (find_unit(b, name)) return 1;
    if (b->count >= FE_BUILD_UNIT_MAX) {
        fe_diag_error(b->diags, from, "too many units in one build");
        return 0;
    }
    if (strlen(name) >= FE_UNIT_PATH_MAX) {
        fe_diag_errorf(b->diags, from, "unit path is too long: %s", name);
        return 0;
    }
    unit = &b->units[b->count];
    memset(unit, 0, sizeof *unit);
    strcpy(unit->name, name);
    /* `std` is reserved (SPEC 10) and lives with the compiler, not with the
       program, so it is looked up under its own root. */
    unit_source_path(unit->path, sizeof unit->path,
                     (name[0]=='s' && name[1]=='t' && name[2]=='d' &&
                      (name[3]=='.' || name[3]==0) && b->std_root[0])
                         ? b->std_root : b->root,
                     name);
    unit->source = read_source(unit->path, &size);
    if (!unit->source) {
        if (have_from)
            fe_diag_errorf(b->diags, from, "import %s has no source file", name);
        else
            fe_diag_errorf(b->diags, from, "cannot open %s", unit->path);
        return 0;
    }
    b->count++;
    unit->size = size;
    fe_ast_init(&unit->ast);
    /* Diagnostics from here on belong to this file. */
    fe_diags_source(b->diags, unit->source, size);
    fe_parser_init(&p, &unit->ast, unit->source, size, unit->path, b->diags);
    unit->ast.root = fe_parse_unit(&p);
    unit->loaded = 1;
    if (!fe_resolve_unit_identity(&unit->ast, b->diags, unit->path)) ok = 0;

    stack[depth] = unit->name;
    for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next) {
        if (n->kind != FE_N_IMPORT || !n->text) continue;
        /* The import statement is where the reader has to make a change, so
           the diagnostic points there rather than at the unit it names. */
        if (!load_unit(b, n->text, n->loc, 1, stack, depth + 1)) ok = 0;
        fe_diags_source(b->diags, unit->source, unit->size);
    }
    stack[depth] = 0;
    return ok;
}

/* A binding names one unit inside one importer; two imports cannot claim it. */
static int check_bindings(FeBuild *b, FeUnit *unit)
{
    FeNode *n, *m;
    int ok = 1;
    for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next) {
        const char *a;
        if (n->kind != FE_N_IMPORT) continue;
        a = fe_import_binding(n);
        if (!a) continue;
        for (m = unit->ast.root->children; m != n; m = m->next) {
            const char *other;
            if (m->kind != FE_N_IMPORT) continue;
            other = fe_import_binding(m);
            if (other && strcmp(a, other) == 0) {
                fe_diag_errorf(b->diags, n->loc,
                               "import binding %s is already taken; use an alias", a);
                ok = 0;
            }
        }
    }
    return ok;
}

int fe_build_load(FeBuild *build, const char *entry, FeDiags *diags,
                  const char *std_root)
{
    const char *stack[FE_BUILD_UNIT_MAX];
    FeAst probe;
    FeParser p;
    char *source;
    char name[FE_UNIT_PATH_MAX];
    FeLoc loc;
    unsigned long size;
    unsigned i;
    int ok;

    memset(build, 0, sizeof *build);
    build->diags = diags;
    if (std_root) {
        unsigned long k = 0;
        while (std_root[k] && k + 1 < sizeof build->std_root) {
            build->std_root[k] = std_root[k];
            ++k;
        }
        build->std_root[k] = 0;
    }

    /* The entry file fixes the import root, so it has to be parsed far enough
       to know its own name before anything else can be found. */
    source = read_source(entry, &size);
    if (!source) {
        FeLoc none;
        none.file = entry; none.line = 0; none.col = 0;
        fe_diag_errorf(diags, none, "cannot open %s", entry);
        return 0;
    }
    fe_ast_init(&probe);
    fe_parser_init(&p, &probe, source, size, entry, diags);
    probe.root = fe_parse_unit(&p);
    if (!probe.root || !probe.root->text || diags->errors) {
        fe_ast_destroy(&probe);
        free(source);
        return 0;
    }
    import_root(build->root, sizeof build->root, entry, probe.root->text);
    strncpy(name, probe.root->text, sizeof name - 1);
    name[sizeof name - 1] = 0;
    loc = probe.root->loc;
    fe_ast_destroy(&probe);
    free(source);

    ok = load_unit(build, name, loc, 0, stack, 0);
    for (i = 0; i < build->count; ++i)
        if (!check_bindings(build, &build->units[i])) ok = 0;
    return ok && diags->errors == 0;
}

void fe_build_destroy(FeBuild *build)
{
    unsigned i;
    for (i = 0; i < build->count; ++i) {
        if (build->units[i].loaded) fe_ast_destroy(&build->units[i].ast);
        free(build->units[i].source);
    }
    build->count = 0;
}

FeUnit *fe_build_binding(FeBuild *build, FeUnit *unit, const char *binding)
{
    FeNode *n;
    for (n = unit->ast.root ? unit->ast.root->children : 0; n; n = n->next) {
        const char *bound;
        if (n->kind != FE_N_IMPORT || !n->text) continue;
        bound = fe_import_binding(n);
        if (bound && strcmp(bound, binding) == 0) return find_unit(build, n->text);
    }
    return 0;
}
