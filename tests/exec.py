"""Compile the programs under `fec/tests/exec/`, run them, and check what they do.

A program says what it should do in its first lines:

    // EXIT:55            the process must exit with this code
    // OUTPUT:hello       this text must appear in what it wrote
    // NOCHECKS:0         build it a second time with --no-checks and expect
                          this exit code instead

The point of this suite is different from `run.py`. That one checks what the
compiler says about a program; this one checks what the program does. A bounds
check that is reported but never emitted passes there and fails here.
"""
from __future__ import annotations

import argparse
import re
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as builder  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PROGRAMS = ROOT / "fec" / "tests" / "exec"


def expectations(path: Path) -> dict:
    want = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("//"):
            break
        m = re.match(r"//\s*(EXIT|OUTPUT|NOCHECKS):(.*)", line)
        if m:
            key, value = m.group(1), m.group(2).strip()
            want[key] = int(value) if key in ("EXIT", "NOCHECKS") else value
    return want


def check_one(fec: Path, path: Path, out_dir: Path) -> tuple[bool, str]:
    want = expectations(path)
    if "EXIT" not in want:
        return False, "no // EXIT: marker"

    exe, log = builder.build(fec, path, out_dir)
    if not exe:
        detail = "\n".join(f"  {n} (exit {c}): {t.strip()}"
                           for n, c, t in log if c != 0 or t.strip())
        return False, "did not build\n" + detail
    code, text = builder.run(exe)
    if code != want["EXIT"]:
        return False, f"exited {code}, expected {want['EXIT']}\n  {text.strip()}"
    if "OUTPUT" in want and want["OUTPUT"] not in text:
        return False, f"output has no {want['OUTPUT']!r}\n  {text.strip()}"

    if "NOCHECKS" in want:
        exe2, log2 = builder.build(fec, path, out_dir / "nochecks",
                                   no_checks=True)
        if not exe2:
            return False, "did not build with --no-checks"
        code2, _ = builder.run(exe2)
        if code2 != want["NOCHECKS"]:
            return False, (f"--no-checks exited {code2}, expected "
                           f"{want['NOCHECKS']}")
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser(description="run the compiled programs")
    ap.add_argument("-k", dest="select")
    ap.add_argument("-v", dest="verbose", action="store_true")
    args = ap.parse_args()

    fec = ROOT / ".build" / "fec.exe"
    if not fec.is_file():
        print("build the front end first: uv run python tests/run.py")
        return 2
    out_dir = ROOT / ".build" / "exec"
    if out_dir.exists():
        shutil.rmtree(out_dir, ignore_errors=True)

    cases = sorted(PROGRAMS.rglob("*.fe"))
    if args.select:
        cases = [p for p in cases if args.select in p.as_posix()]
    if not cases:
        print("no programs found")
        return 1

    failed = []
    for path in cases:
        ok, why = check_one(fec, path, out_dir / path.stem)
        rel = path.relative_to(PROGRAMS).as_posix()
        if ok:
            if args.verbose:
                print(f"  ok   {rel}")
        else:
            failed.append((rel, why))
    for rel, why in failed:
        print(f"FAIL {rel}: {why}")
    print(f"\n{len(cases) - len(failed)}/{len(cases)} programs behaved")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
