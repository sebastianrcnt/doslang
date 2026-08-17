"""Compile a Ferro program to a Windows executable and run it.

    fec --emit-asm  ->  wasm  ->  wlink (+ the runtime, + kernel32)  ->  .exe

The toolchain is the pinned Open Watcom under `.dosboxx/watcom`, hosted: the
assembler and linker there produce PE binaries as happily as they produce DOS
ones. Nothing about this step needs a virtual machine.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WATCOM = ROOT / ".dosboxx" / "watcom"
RUNTIME = ROOT / "fec" / "rt" / "start.asm"


def _env() -> dict:
    env = dict(os.environ)
    env.update(WATCOM=str(WATCOM),
               INCLUDE=f"{WATCOM / 'h'};{WATCOM / 'h' / 'nt'}",
               PATH=f"{WATCOM / 'binnt'}{os.pathsep}{env.get('PATH', '')}")
    return env


def _run(cmd, cwd) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], cwd=cwd, env=_env(),
                          capture_output=True, text=True, timeout=120)


def _rel(p: Path) -> str:
    """ROOT-relative, with forward slashes, so the spelling does not depend on
    which machine or shell ran the build."""
    return Path(p).resolve().relative_to(ROOT).as_posix()


def build(fec: Path, source: Path, out_dir: Path, no_checks: bool = False):
    """Returns (exe_path, log). exe_path is None when a step failed."""
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = source.stem
    asm = out_dir / f"{stem}.asm"
    log = []

    # `std` ships with the compiler, so it is looked for beside it rather
    # than beside the program.
    #
    # The paths go in relative, from ROOT. `fec` records the path it was told
    # -- that is what a trap prints and so what lands in the executable -- so
    # an absolute path would put this machine's directory layout in the binary
    # and no two machines would build the same bytes. See `tests/determ.py`.
    cmd = [fec, "--emit-asm", _rel(source), "-o", asm.resolve(), "--std=fec"]
    if no_checks:
        cmd.append("--no-checks")
    step = _run(cmd, ROOT)
    log.append(("fec", step.returncode, step.stdout + step.stderr))
    if step.returncode != 0 or not asm.is_file():
        return None, log

    wasm = WATCOM / "binnt" / "wasm.exe"
    for src, obj in ((asm, out_dir / f"{stem}.obj"),
                     (RUNTIME, out_dir / "start.obj")):
        step = _run([wasm, "-q", "-zq", src, f"-fo={obj}"], out_dir)
        log.append(("wasm " + src.name, step.returncode,
                    step.stdout + step.stderr))
        if step.returncode != 0:
            return None, log

    exe = out_dir / f"{stem}.exe"
    step = _run([WATCOM / "binnt" / "wlink.exe",
                 "system", "nt",
                 "file", out_dir / f"{stem}.obj",
                 "file", out_dir / "start.obj",
                 "library", WATCOM / "lib386" / "nt" / "kernel32.lib",
                 "name", exe,
                 "option", "quiet"], out_dir)
    log.append(("wlink", step.returncode, step.stdout + step.stderr))
    if step.returncode != 0 or not exe.is_file():
        return None, log
    return exe, log


def run(exe: Path):
    done = subprocess.run([str(exe)], capture_output=True, text=True,
                          timeout=30)
    return done.returncode, done.stdout + done.stderr


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: build.py <program.fe> [--no-checks]")
        return 2
    source = Path(sys.argv[1]).resolve()
    fec = ROOT / ".build" / "fec.exe"
    if not fec.is_file():
        print("build the front end first: uv run python tests/run.py")
        return 2
    exe, log = build(fec, source, ROOT / ".build" / "out",
                     "--no-checks" in sys.argv)
    for name, code, text in log:
        if code != 0 or text.strip():
            print(f"--- {name} (exit {code})")
            print(text.rstrip())
    if not exe:
        return 1
    code, text = run(exe)
    if text:
        print(text, end="")
    print(f"{exe.name} exited {code}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
