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
