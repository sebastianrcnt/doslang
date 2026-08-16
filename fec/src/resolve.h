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

/* Validate the `unit` declaration against SPEC 8.1, and against the file it was
   read from: the path must match the dotted name, so `game.world.map` has to
   come from `game/world/map.fe`. `source_path` may be null to skip that half.
   Returns non-zero when the unit is well formed. */
int fe_resolve_unit_identity(FeAst *ast, FeDiags *diags, const char *source_path);

#endif
