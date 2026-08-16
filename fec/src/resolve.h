#ifndef FE_RESOLVE_H
#define FE_RESOLVE_H

#include "ast.h"
#include "diag.h"

/* Unit-level resolution: identity, import bindings, and the unit graph.
   This runs between parsing and semantic checking. It answers questions that
   need more than one file -- what a unit is called, what it imports, and
   whether those imports exist and terminate -- so that check.c can keep
   looking at one function at a time. */

/* SPEC 8.1: each segment is ASCII lowercase, starts with a letter, continues
   with letters, digits or '_', and is at most eight characters. The limit is
   what makes a unit path map to a FAT/DOS 8.3 source path unambiguously. */
#define FE_UNIT_SEGMENT_MAX 8
#define FE_UNIT_PATH_MAX 128
#define FE_BUILD_UNIT_MAX 64

typedef struct FeUnit {
    char name[FE_UNIT_PATH_MAX];   /* canonical dotted path */
    char path[260];                /* source file it was read from */
    FeAst ast;
    char *source;                  /* owned; freed with the build */
    unsigned long size;
    int loaded;
    int checked;
} FeUnit;

typedef struct FeBuild {
    FeUnit units[FE_BUILD_UNIT_MAX];
    unsigned count;
    char root[260];                /* import root: where unit paths start */
    FeDiags *diags;
} FeBuild;

/* Validate the `unit` declaration against SPEC 8.1, and against the file it was
   read from: the path must match the dotted name, so `game.world.map` has to
   come from `game/world/map.fe`. `source_path` may be null to skip that half.
   Returns non-zero when the unit is well formed. */
int fe_resolve_unit_identity(FeAst *ast, FeDiags *diags, const char *source_path);

/* Load `entry` and everything it imports, transitively.

   The import root is derived from the entry file: a unit named `a.b` read from
   `<root>/a/b.fe` fixes `<root>`, so a sibling `import c.d;` is looked for at
   `<root>/c/d.fe`. Reports missing imports, import cycles, and binding
   conflicts. Returns non-zero when the whole graph loaded cleanly. */
int fe_build_load(FeBuild *build, const char *entry, FeDiags *diags);
void fe_build_destroy(FeBuild *build);

/* The unit a binding refers to inside `unit`, or null.
   The binding is the last segment of the import path unless `as` renamed it. */
FeUnit *fe_build_binding(FeBuild *build, FeUnit *unit, const char *binding);

/* The local name an import introduces: its alias, or the last path segment. */
const char *fe_import_binding(const FeNode *import);

#endif
