"""Does the compiler's output depend on the machine it ran on?

It must not. A compiler whose `.asm` differs by host cannot be checked by
comparing output, and a program built on two machines would not be the same
program. Two properties, checked separately:

  same build, twice      the same `fec` on the same input twice. Catches
                         anything that rides on addresses or on memory that
                         was never written -- hash tables walked in allocation
                         order, uninitialised padding, a clock.

  different builds       `fec` built by a different compiler, or for a
                         different word size, on the same input. Catches the
                         host's integer widths leaking into what the target
                         sees. `long` is 4 bytes when `fec` is built 32-bit
                         and 8 when it is built 64-bit, so a constant like the
                         FNV-1a basis 0x811C9DC5 printed through `%ld` came out
                         -2128831035 one way and 2166136261 the other -- same
                         bits, same instruction, different text. `word.h` is
                         where that was closed.

The second needs two toolchains. Where only one exists this says so rather
than reporting a pass it did not earn.

    uv run python tests/determ.py
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import ROOT, SOURCES, WATCOM  # noqa: E402

WORK = ROOT / ".dosboxx" / "runs" / "determ"
SRC = ROOT / "fec" / "src"


def _watcom_env() -> dict:
    import os
    env = dict(os.environ)
    env.update(WATCOM=str(WATCOM),
               INCLUDE=f"{WATCOM / 'h'};{WATCOM / 'h' / 'nt'}",
               PATH=f"{WATCOM / 'binnt'}{os.pathsep}{env.get('PATH', '')}")
    return env


def build_watcom(out: Path):
    """The pinned Open Watcom, hosted. 32-bit: `long` is 4 bytes."""
    wcl = WATCOM / "binnt" / "wcl386.exe"
    if not wcl.is_file():
        return None
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "fec.exe"
    cmd = [str(wcl), "-q", "-za", "-wx", "-bt=nt", "-fe=fec.exe", f"-i={SRC}"]
    cmd += [str(SRC / f"{n}.c") for n in SOURCES]
    try:
        done = subprocess.run(cmd, cwd=out, capture_output=True, text=True,
                              env=_watcom_env())
    except OSError:
        return None      # a Windows toolchain seen from a Unix host
    return exe if done.returncode == 0 and exe.is_file() else None


def build_cc(out: Path, cc: str, extra: list[str], name: str):
    """A host C compiler. `-m32`/`-m64` is how one machine gets two widths."""
    if not shutil.which(cc):
        return None
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("fec.exe" if sys.platform == "win32" else "fec")
    cmd = [cc, "-O1", "-std=c89", "-w", f"-I{SRC}", "-o", str(exe)]
    cmd += extra + [str(SRC / f"{n}.c") for n in SOURCES]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0 or not exe.is_file():
        return None
    return exe


RECIPES = [
    ("watcom-32", lambda o: build_watcom(o)),
    ("cc-native", lambda o: build_cc(o, "cc", [], "cc-native")),
    ("cc-32", lambda o: build_cc(o, "cc", ["-m32"], "cc-32")),
    ("gcc-native", lambda o: build_cc(o, "gcc", [], "gcc-native")),
    ("gcc-32", lambda o: build_cc(o, "gcc", ["-m32"], "gcc-32")),
]


def fixtures() -> list[Path]:
    return sorted((ROOT / "fec" / "tests" / "exec").rglob("*.fe"))


def emit(fec: Path, source: Path, dest: Path) -> bool:
    """Paths go in ROOT-relative: `fec` records what it was told, and a trap
    prints it, so an absolute path would put this machine into the output."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    rel = source.resolve().relative_to(ROOT).as_posix()
    done = subprocess.run(
        [str(fec), "--emit-asm", rel, "-o", str(dest), "--std=fec"],
        cwd=ROOT, capture_output=True, text=True, timeout=120)
    return done.returncode == 0 and dest.is_file()


def emit_all(fec: Path, out: Path, sources: list[Path]) -> dict[str, bytes]:
    got = {}
    for fe in sources:
        key = fe.resolve().relative_to(ROOT).as_posix()
        dest = out / (key.replace("/", "__").replace(".fe", ".asm"))
        if emit(fec, fe, dest):
            got[key] = dest.read_bytes()
    return got


def compare(label: str, a: dict[str, bytes], b: dict[str, bytes]) -> int:
    shared = sorted(set(a) & set(b))
    bad = [k for k in shared if a[k] != b[k]]
    only = sorted(set(a) ^ set(b))
    for k in bad[:5]:
        print(f"  DIFFERS  {k}")
        la, lb = a[k].decode(errors="replace").splitlines(), \
            b[k].decode(errors="replace").splitlines()
        for i, (x, y) in enumerate(zip(la, lb)):
            if x != y:
                print(f"    line {i + 1}\n      < {x.strip()}\n      > {y.strip()}")
                break
    for k in only[:5]:
        print(f"  ONLY ONE SIDE COMPILED IT  {k}")
    n = len(bad) + len(only)
    print(f"  {label}: {len(shared) - len(bad)}/{len(shared)} identical"
          + (f", {len(only)} compiled by only one side" if only else ""))
    return n


def main() -> int:
    sources = fixtures()
    if not sources:
        sys.exit("no fixtures")
    print(f"{len(sources)} programs\n")

    # `cc` and `gcc` are usually the same compiler, and comparing a build
    # against itself would report a pass it did not earn. Byte-identical
    # binaries are one build under two names.
    built, seen = [], {}
    for name, make in RECIPES:
        exe = make(WORK / name)
        if not exe:
            continue
        digest = hashlib.sha256(exe.read_bytes()).hexdigest()
        if digest in seen:
            print(f"  ({name} builds the same binary as {seen[digest]})")
            continue
        seen[digest] = name
        built.append((name, exe))
    if not built:
        sys.exit("no C compiler available to build fec")

    # Building is not the same as working. A Windows `fec.exe` will build and
    # even start under WSL's binfmt, and then emit nothing useful; a recipe
    # that cannot produce output has nothing to compare.
    working = []
    for name, exe in built:
        got = emit_all(exe, WORK / name / "a", sources)
        if len(got) == len(sources):
            working.append((name, exe, got))
        else:
            print(f"  ({name} built but emitted {len(got)}/{len(sources)}"
                  f" -- not usable here, dropped)")
    if not working:
        sys.exit("no build of fec can emit on this machine")

    failures = 0

    print("\nsame build, twice")
    name, exe, first = working[0]
    second = emit_all(exe, WORK / name / "b", sources)
    failures += compare(name, first, second)

    print("\ndifferent builds")
    if len(working) < 2:
        print(f"  only {working[0][0]} is usable here -- SKIPPED.")
        print("  Install a second C compiler (or gcc-multilib for -m32) to")
        print("  check that the host's word size stays out of the output.")
    else:
        base_name, base = working[0][0], first
        for other_name, _, got in working[1:]:
            failures += compare(f"{base_name} vs {other_name}", base, got)

    print()
    if failures:
        print(f"{failures} outputs depend on the host")
        return 1
    print("output does not depend on the host")
    return 0


if __name__ == "__main__":
    sys.exit(main())
