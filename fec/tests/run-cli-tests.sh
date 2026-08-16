#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

"$root"/fec --dump-tokens "$root"/tests/pass/basic.fe >"$tmp/tokens.out"
grep 'unit' "$tmp/tokens.out" >/dev/null
grep 'identifier' "$tmp/tokens.out" >/dev/null
grep 'eof' "$tmp/tokens.out" >/dev/null

"$root"/fec --check "$root"/tests/m2/hello.fe >"$tmp/check.out"
if [ -s "$tmp/check.out" ]; then
    echo "FAIL: --check produced stdout"
    exit 1
fi

if "$root"/fec --check "$root"/tests/m2/bad-condition.fe >"$tmp/bad.out" 2>"$tmp/diag.out"; then
    echo "FAIL: --check accepted invalid input"
    exit 1
fi
grep 'error:' "$tmp/diag.out" >/dev/null
grep '^  [0-9][0-9]* | ' "$tmp/diag.out" >/dev/null
grep ' | .*\^' "$tmp/diag.out" >/dev/null

echo "CLI tests: token dump, check mode, and source diagnostics passed"
