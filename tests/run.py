"""Run every fixture through the front end and check what it reports.

A fixture is checked by running `fec` on it and looking at two things: whether
it was accepted, and, when it was rejected, whether the diagnostic is the one
the fixture asked for.

A fixture states its expectation in its first line:

    // ERROR:8:self          rejected at line 8, with "self" in the message
    // ERROR:expected ';'    rejected, message only -- the parse fixtures, where
                             the line is not the interesting part

A fixture with no marker whose name starts with `bad` must be rejected but does
not pin the message yet. Anything else must be accepted.

Fixtures under `parse/` are checked with --dump-ast rather than --check: they
exercise the grammar, and several are deliberately not well-typed.

This runs on the host in about a second. It checks what the compiler says; what
the compiled programs actually do is `exec.py`.
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
           "check", "checkexp", "checkstm", "checkgen", "checkcal", "checkpro",
           "resolve", "ir", "lower", "lowerprn", "lowerexp", "lowerstm", "x86", "report", "driver")

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
    done = subprocess.run([str(fec), mode, str(path),
                           f"--std={ROOT / 'fec'}"],
                          capture_output=True, text=True, timeout=30)
    output = (done.stdout + done.stderr).strip()
    # Diagnostics go to the diag stream. `--dump-ast` also writes the tree to
    # stdout, so a marker matched against the two together would read the tree
    # and never reach the error.
    diags = done.stderr.strip() or output
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
    first = diags.split("\n", 1)[0] if diags else ""
    at = re.search(r":(\d+):\d+: error:", first)
    if not at:
        return False, f"no diagnostic to match marker\n  got: {first or '(silent)'}"
    if int(at.group(1)) != want.line:
        return False, f"marker says line {want.line}, diagnostic is line {at.group(1)}\n  {first}"
    if want.text and want.text.lower() not in diags.lower():
        return False, f"marker wants {want.text!r}\n  {first}"
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser(description="run the front-end fixtures")
    ap.add_argument("-k", dest="select", help="only fixtures whose path contains this")
    ap.add_argument("-v", dest="verbose", action="store_true")
    args = ap.parse_args()

    fec = build(ROOT / ".build")
    cases = sorted(FIXTURES.rglob("*.fe"))
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
    if not args.select:
        leak = unsafe_budget(fec)
        if leak:
            print(leak)
            return 1
    return 1 if failed else 0


# The programs the budget is measured on: the Ferro front end, and the ones
# that lean hardest on the standard library.
BUDGETED = ("exec/lexer/tree.fe", "exec/interns.fe", "exec/arenat.fe",
            "exec/maps.fe", "exec/wordfreq.fe")


def unsafe_budget(fec: Path) -> str:
    """`unsafe` and `*T` belong to std.mem and std.sys. Anywhere else they are
    a hole in what the checker promises, so the count outside std has to stay
    at zero and a regression has to fail the build rather than be noticed."""
    for rel in BUDGETED:
        path = FIXTURES / rel
        if not path.is_file():
            return f"budget: {rel} is gone"
        done = subprocess.run([str(fec), "--report-unsafe", str(path),
                               f"--std={ROOT / 'fec'}"],
                              capture_output=True, text=True)
        line = [l for l in done.stdout.splitlines()
                if l.startswith("outside std")]
        if not line:
            return f"budget: no report for {rel}\n{done.stdout}{done.stderr}"
        counts = line[0].split()[2:]
        if any(c != "0" for c in counts):
            return (f"budget: {rel} has unsafe/raw pointers outside std: "
                    f"{line[0]}")
    return ""


if __name__ == "__main__":
    raise SystemExit(main())
