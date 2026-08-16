"""Compile the compiler's own sources on the host, as a syntax gate.

This is not verification. AGENTS.md is explicit that a host compiler's result
does not count, and it still does not: the DOS build and the milestone suite
decide whether anything works. What this buys is the turnaround. A missing
declaration or a signature that disagrees with its definition used to surface
only after a DOSBox-X boot and a full compiler build; here it surfaces in about
a second, with the line number.

It uses the same Open Watcom the project targets, just the Windows-hosted build,
and the same strictness as ``fec/build-dos.bat`` (``-za -wx -wcd=202``). The
target differs -- wcc386 is 32-bit where the DOS build is 16-bit large model --
so this catches syntax, types and declarations, not code generation or memory
model problems.

Skipped when Watcom is not installed, so the suite still runs anywhere.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from ferrolang_vm.paths import ROOT

SRC = ROOT / "fec" / "src"
# Mirrors the compile order in fec/build-dos.bat.
SOURCES = ("arena", "diag", "lexer", "ast", "parser", "types", "m7", "own",
           "check", "lower", "emit_c", "driver")


def _watcom() -> Path | None:
    root = os.environ.get("WATCOM")
    candidates = [Path(root)] if root else []
    candidates.append(Path("C:/WATCOM19"))
    for base in candidates:
        if (base / "binnt" / "wcc386.exe").is_file():
            return base
    return None


@pytest.fixture(scope="session")
def watcom() -> Path:
    base = _watcom()
    if base is None:
        pytest.skip("Open Watcom is not installed on the host; set WATCOM to enable")
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
    completed = subprocess.run(
        [os.fspath(watcom / "binnt" / "wcc386.exe"), "-q", "-za", "-wx",
         "-wcd=202", "-zq", f"-i={SRC}", os.fspath(source)],
        cwd=objdir, capture_output=True, text=True, env=env, timeout=120,
    )
    output = (completed.stdout + completed.stderr).strip()
    # -wx keeps warnings meaningful, so treat any diagnostic as a failure: the
    # DOS build runs the same flags and stops on them.
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
