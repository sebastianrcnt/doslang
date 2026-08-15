#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ok=0
for f in "$root"/std/*.fe; do
    [ -f "$f" ] || continue
    "$root"/fec --dump-ast "$f" >/dev/null || { echo "FAIL: $f"; exit 1; }
    ok=$((ok+1))
done
for f in "$root"/tests/pass/*.fe; do
    [ -f "$f" ] || continue
    "$root"/fec --dump-ast "$f" >/dev/null || { echo "FAIL: $f"; exit 1; }
    ok=$((ok+1))
done
for f in "$root"/tests/fail/*.fe; do
    [ -f "$f" ] || continue
    if "$root"/fec --dump-ast "$f" >/dev/null 2>/dev/null; then echo "FAIL (accepted): $f"; exit 1; fi
    ok=$((ok+1))
done
echo "M1 tests: $ok cases passed"

m2tmp=$(mktemp -d)
trap 'rm -rf "$m2tmp"' EXIT HUP INT TERM
"$root"/fec --emit-c "$root"/tests/m2/hello.fe -o "$m2tmp/hello.c"
${CC:-cc} -std=c89 -pedantic "$m2tmp/hello.c" -o "$m2tmp/hello"
"$m2tmp/hello"
"$root"/fec --target=bits16 --emit-c "$root"/tests/m2/hello.fe -o "$m2tmp/hello16.c"
${CC:-cc} -std=c89 -pedantic "$m2tmp/hello16.c" -o "$m2tmp/hello16"
"$m2tmp/hello16"
"$root"/fec --emit-c "$root"/tests/m2/cast-while.fe -o "$m2tmp/cast.c"
${CC:-cc} -std=c89 -pedantic "$m2tmp/cast.c" -o "$m2tmp/cast"
"$m2tmp/cast"
"$root"/fec --emit-c "$root"/tests/m2/scopes.fe -o "$m2tmp/scopes.c"
${CC:-cc} -std=c89 -pedantic "$m2tmp/scopes.c" -o "$m2tmp/scopes"
"$m2tmp/scopes"
for f in bad-condition bad-cast bad-assign bad-unknown bad-arity bad-types bad-return bad-uninit bad-void; do
    if "$root"/fec --emit-c "$root"/tests/m2/$f.fe -o "$m2tmp/$f.c" >/dev/null 2>/dev/null; then
        echo "FAIL (accepted M2 semantic error): $f.fe"
        exit 1
    fi
done
echo "M2 tests: integer control-flow smoke passed"

m3tmp=$(mktemp -d)
trap 'rm -rf "$m2tmp" "$m3tmp"' EXIT HUP INT TERM
for f in struct enum array arrayctx str for nested char; do
    "$root"/fec --target=bits32 --emit-c "$root"/tests/m3/$f.fe -o "$m3tmp/$f.c"
    ${CC:-cc} -std=c89 -pedantic "$m3tmp/$f.c" -o "$m3tmp/$f"
    "$m3tmp/$f"
done
"$root"/fec --target=bits32 --emit-c "$root"/tests/m3/bounds.fe -o "$m3tmp/bounds.c"
${CC:-cc} -std=c89 -pedantic "$m3tmp/bounds.c" -o "$m3tmp/bounds"
if "$m3tmp/bounds"; then
    echo "FAIL (bounds trap did not fire): tests/m3/bounds.fe"
    exit 1
fi
for f in badfld badmat badarr badcycle badstr badchar badfield badindex; do
    if "$root"/fec --target=bits32 --emit-c "$root"/tests/m3/$f.fe -o "$m3tmp/$f.c" >/dev/null 2>/dev/null; then
        echo "FAIL (accepted M3 semantic error): $f.fe"
        exit 1
    fi
done
echo "M3 tests: structs, enums, arrays, slices, str, match, and bounds passed"
