#include "resolve.h"

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
