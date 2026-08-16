"""Compile the compiler's own sources on the host, as a syntax gate.

This is not verification. AGENTS.md is explicit that a host compiler's result
does not count, and it still does not: the DOS build and the milestone suite
decide whether anything works. What this buys is the turnaround. A missing
declaration or a signature that disagrees with its definition used to surface
only after a DOSBox-X boot and a full compiler build; here it surfaces in about
a second, with the line number.

It runs the pinned toolchain's Windows-hosted 16-bit driver with the exact
command ``fec/build-dos.bat`` uses, so the diagnostics match what the DOS build
sees. A 32-bit compile is not equivalent: it misses warnings that only the
16-bit large model reports, which is how a dead function survived the M7
unification with a clean 32-bit check.

What it still cannot see is the DOS environment itself -- memory limits, the
command line length, the filesystem. The DOS build and the milestone suite
remain the gate.

It uses the pinned toolchain only. There is no environment override and no
skip: a system-wide Watcom is a different version reporting different things,
and a gate that quietly skips is not a gate.
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

from ferrolang_vm.paths import ROOT

SRC = ROOT / "fec" / "src"
# Mirrors the compile order in fec/build-dos.bat.
SOURCES = ("arena", "diag", "lexer", "ast", "parser", "types", "m7", "own",
           "check", "lower", "emit_c", "driver")


@pytest.fixture(scope="session")
def watcom() -> Path:
    """The pinned toolchain, and nothing else.

    Deliberately no environment override and no skip. A system-wide Open Watcom
    is a different version with different diagnostics, and a gate that skips is
    a gate that is not running -- which is the failure mode this file exists to
    close. dosboxx.py fails the same way when the toolchain is missing.
    """
    base = ROOT / ".dosboxx" / "watcom"
    if not (base / "binnt" / "wcl.exe").is_file():
        raise AssertionError(
            f"the pinned Open Watcom is not at {base}; "
            "run `uv run ferro-dos setup --accept-watcom-license`")
    return base


@pytest.fixture(scope="session")
def objdir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("wcc")


@pytest.mark.parametrize("name", SOURCES)
def test_source_compiles_clean(name: str, watcom: Path, objdir: Path) -> None:
    source = SRC / f"{name}.c"
    if not source.is_file():
        pytest.fail(f"{source} is missing but build-dos.bat compiles it")
    env = dict(os.environ)
    env["WATCOM"] = os.fspath(watcom)
    env["INCLUDE"] = os.fspath(watcom / "h")
    env["PATH"] = os.pathsep.join(
        [os.fspath(watcom / "binnt"), env.get("PATH", "")])
    # The same command build-dos.bat runs, minus the object name.
    completed = subprocess.run(
        [os.fspath(watcom / "binnt" / "wcl.exe"), "-q", "-za", "-wx",
         "-bt=dos", "-ml", "-k32768", "-c", f"-i={SRC}", os.fspath(source)],
        cwd=objdir, capture_output=True, text=True, env=env, timeout=120,
    )
    output = (completed.stdout + completed.stderr).strip()
    # -wx keeps warnings meaningful, so treat any diagnostic as a failure. The
    # DOS build prints them to a screen nobody reads, which is how they
    # accumulate unnoticed.
    assert completed.returncode == 0 and not output, (
        f"{name}.c does not compile clean\n{output}"
    )


def test_build_scripts_agree_on_sources() -> None:
    """The two build files and this test must name the same translation units.

    They drifted apart while the M7 wrapper existed, which is how a source could
    stop being compiled without anyone noticing.
    """
    batch = (ROOT / "fec" / "build-dos.bat").read_text(encoding="utf-8", errors="replace")
    makefile = (ROOT / "fec" / "Makefile").read_text(encoding="utf-8", errors="replace")
    in_batch = set(re.findall(r"src\\(\w+)\.c", batch))
    srcline = next(line for line in makefile.splitlines() if line.startswith("SRC ="))
    in_make = set(re.findall(r"src/(\w+)\.c", srcline))
    assert in_batch == set(SOURCES), f"build-dos.bat compiles {sorted(in_batch)}"
    assert in_make == set(SOURCES), f"Makefile compiles {sorted(in_make)}"


def test_no_source_is_orphaned() -> None:
    """Every .c under fec/src must be compiled by something.

    check_m7.c and emitcm7.c hid check.c and emit_c.c from the build by
    including them textually; nothing flagged that they had stopped being
    translation units of their own.
    """
    on_disk = {p.stem for p in SRC.glob("*.c")}
    assert on_disk == set(SOURCES), (
        f"fec/src has {sorted(on_disk - set(SOURCES))} that no build step compiles"
    )
