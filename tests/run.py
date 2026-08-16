"""Run every fixture through the front end and check what it reports.

The compiler is a front end now -- lexer, parser, types, ownership -- so a
fixture is checked by running `fec` on it and looking at two things: whether it
was accepted, and, when it was rejected, whether the diagnostic is the one the
fixture asked for.

A fixture states its expectation in its first line:

    // ERROR:8:self          rejected at line 8, with "self" in the message
    // ERROR:expected ';'    rejected, message only -- the parse fixtures, where
                             the line is not the interesting part

A fixture with no marker whose name starts with `bad` must be rejected but does
not pin the message yet. Anything else must be accepted.

Fixtures under `parse/` are checked with --dump-ast rather than --check: they
exercise the grammar, and several are deliberately not well-typed.

This runs on the host in about a second. There is no VM: nothing here executes
generated code, because there is no code generator.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "fec" / "tests"
WATCOM = ROOT / ".dosboxx" / "watcom"
SOURCES = ("arena", "diag", "lexer", "ast", "parser", "types", "m7", "own",
           "check", "driver")
# Fixtures live here until there is a code generator to run them against.
QUARANTINE = "pending-backend"

MARKER = re.compile(r"^//\s*ERROR:(?:(\d+):)?(.*)$")


@dataclass
class Expectation:
    rejected: bool
    line: int | None = None
    text: str | None = None


def expectation(path: Path) -> Expectation:
    first = path.read_text(encoding="utf-8", errors="replace").split("\n", 1)[0]
    m = MARKER.match(first.strip())
    if m:
        line = int(m.group(1)) if m.group(1) else None
        return Expectation(True, line, m.group(2).strip())
    name = path.stem
    return Expectation(name.startswith("bad") or "-bad-" in name or name.startswith("own-bad"))


def build(out: Path) -> Path:
    """Build the front end with the pinned toolchain, hosted."""
    wcl = WATCOM / "binnt" / "wcl386.exe"
    if not wcl.is_file():
        sys.exit(f"pinned Open Watcom not found at {WATCOM}")
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update(WATCOM=str(WATCOM), INCLUDE=f"{WATCOM / 'h'};{WATCOM / 'h' / 'nt'}",
               PATH=f"{WATCOM / 'binnt'}{os.pathsep}{env.get('PATH', '')}")
    src = ROOT / "fec" / "src"
    cmd = [str(wcl), "-q", "-za", "-wx", "-bt=nt", "-fe=fec.exe", f"-i={src}"]
    cmd += [str(src / f"{n}.c") for n in SOURCES]
    done = subprocess.run(cmd, cwd=out, capture_output=True, text=True, env=env)
    if done.returncode != 0 or (done.stdout + done.stderr).strip():
        sys.exit("front end does not build clean:\n" + done.stdout + done.stderr)
    return out / "fec.exe"


def run_case(fec: Path, path: Path) -> tuple[bool, str]:
    want = expectation(path)
    # The grammar fixtures are not all well-typed; stop after parsing.
    mode = "--dump-ast" if path.parent.name == "parse" else "--check"
    done = subprocess.run([str(fec), mode, str(path)],
                          capture_output=True, text=True, timeout=30)
    output = (done.stdout + done.stderr).strip()
    rejected = done.returncode != 0

    if want.rejected != rejected:
        verb = "accepted" if rejected else "rejected"
        return False, f"expected to be {'rejected' if want.rejected else verb}"
    if not want.rejected:
        return True, ""
    if want.line is None:
        if want.text and want.text.lower() not in output.lower():
            got = output.splitlines()[0] if output else "(silent)"
            return False, f"marker wants {want.text!r}\n  {got}"
        return True, ""
    # The marker pins where and roughly what, so a rule can be moved or reworded
    # only deliberately.
    first = output.split("\n", 1)[0] if output else ""
    at = re.search(r":(\d+):\d+: error:", first)
    if not at:
        return False, f"no diagnostic to match marker\n  got: {first or '(silent)'}"
    if int(at.group(1)) != want.line:
        return False, f"marker says line {want.line}, diagnostic is line {at.group(1)}\n  {first}"
    if want.text and want.text.lower() not in output.lower():
        return False, f"marker wants {want.text!r}\n  {first}"
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser(description="run the front-end fixtures")
    ap.add_argument("-k", dest="select", help="only fixtures whose path contains this")
    ap.add_argument("-v", dest="verbose", action="store_true")
    args = ap.parse_args()

    fec = build(ROOT / ".build")
    cases = sorted(p for p in FIXTURES.rglob("*.fe") if QUARANTINE not in p.parts)
    if args.select:
        cases = [p for p in cases if args.select in p.as_posix()]

    failed = []
    for path in cases:
        ok, why = run_case(fec, path)
        rel = path.relative_to(FIXTURES).as_posix()
        if ok:
            if args.verbose:
                print(f"  ok   {rel}")
        else:
            failed.append((rel, why))
    for rel, why in failed:
        print(f"FAIL {rel}: {why}")
    marked = sum(1 for p in cases if expectation(p).line is not None)
    print(f"\n{len(cases) - len(failed)}/{len(cases)} passed "
          f"({marked} pin a line and message)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
