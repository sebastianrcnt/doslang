"""Reproducible DOSBox-X/Open Watcom development test backend."""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

from .paths import ROOT
from .suite import Case


CACHE = ROOT / ".dosboxx"
LOCK_PATH = ROOT / "tools" / "toolchains" / "dosboxx.lock.json"
RUNS = CACHE / "runs"


class DosboxError(RuntimeError):
    pass


def _lock() -> dict[str, object]:
    return json.loads(LOCK_PATH.read_text(encoding="utf-8"))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _download(name: str, spec: dict[str, object]) -> Path:
    downloads = CACHE / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    target = downloads / Path(str(spec["url"])).name
    expected = str(spec["sha256"]).lower()
    if target.is_file() and _sha256(target) == expected:
        return target
    target.unlink(missing_ok=True)
    partial = target.with_suffix(target.suffix + ".part")
    partial.unlink(missing_ok=True)
    print(f"ferro-test: downloading {name} {spec['version']}...")
    try:
        with urllib.request.urlopen(str(spec["url"])) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output, length=1024 * 1024)
    except Exception:
        partial.unlink(missing_ok=True)
        raise
    actual = _sha256(partial)
    if actual != expected:
        partial.unlink(missing_ok=True)
        raise DosboxError(f"{name} SHA-256 mismatch: expected {expected}, got {actual}")
    partial.replace(target)
    return target


def _safe_extract(archive: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{destination.name}-", dir=destination.parent))
    try:
        with zipfile.ZipFile(archive) as bundle:
            root = temporary.resolve()
            for member in bundle.infolist():
                target = (temporary / member.filename).resolve()
                if target != root and root not in target.parents:
                    raise DosboxError(f"unsafe archive member: {member.filename}")
            bundle.extractall(temporary)
        if destination.exists():
            shutil.rmtree(destination)
        temporary.replace(destination)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


def _required_paths(lock: dict[str, object]) -> tuple[Path, Path, list[Path]]:
    dosbox_spec = lock["dosboxx"]
    watcom_spec = lock["open_watcom"]
    assert isinstance(dosbox_spec, dict) and isinstance(watcom_spec, dict)
    dosbox = CACHE / "dosbox-x" / str(dosbox_spec["executable"])
    watcom = CACHE / "watcom"
    required = [watcom / str(item) for item in watcom_spec["required"]]
    return dosbox, watcom, required


def setup(*, accept_watcom_license: bool = False) -> tuple[Path, Path]:
    if os.name != "nt":
        raise DosboxError("the DOSBox-X development backend currently supports Windows only")
    lock = _lock()
    dosbox, watcom, watcom_required = _required_paths(lock)
    tools_ready = dosbox.is_file() and all(path.is_file() for path in watcom_required)
    if not tools_ready and not accept_watcom_license:
        raise DosboxError(
            "Open Watcom is distributed under the Sybase Open Watcom Public License. "
            "Review tools/toolchains/dosboxx.lock.json and rerun "
            "`uv run ferro-dos setup --accept-watcom-license`."
        )
    if not tools_ready:
        dosbox_spec = lock["dosboxx"]
        watcom_spec = lock["open_watcom"]
        assert isinstance(dosbox_spec, dict) and isinstance(watcom_spec, dict)
        _safe_extract(_download("DOSBox-X", dosbox_spec), CACHE / "dosbox-x")
        _safe_extract(_download("Open Watcom", watcom_spec), watcom)
    dosbox, watcom, watcom_required = _required_paths(lock)
    missing = [str(path.relative_to(CACHE)) for path in [dosbox, *watcom_required]
               if not path.is_file()]
    if missing:
        raise DosboxError("toolchain archive is missing: " + ", ".join(missing))
    (CACHE / "SETUP.OK").write_text(_sha256(LOCK_PATH) + "\n", encoding="ascii")
    return dosbox, watcom


def resolve_tools() -> tuple[Path, Path]:
    lock = _lock()
    dosbox, watcom, required = _required_paths(lock)
    if not dosbox.is_file() or not all(path.is_file() for path in required):
        raise DosboxError("toolchain is not installed; run `uv run ferro-dos setup`")
    return dosbox, watcom


# ``if errorlevel N`` in DOS tests ``>= N``, so an exact code needs a descending
# ladder.  Small values get their own rung because they carry the meaning -- 1 is
# an ordinary compiler error, 3 is Watcom's abort() -- while anything above 8 is
# bucketed to a lower bound, which is enough to tell a crash from a diagnostic.
_RC_LADDER = (255, 128, 64, 32, 16, 8, 7, 6, 5, 4, 3, 2, 1)


def _rc_batch() -> str:
    """Batch helper that records the previous command's exit code.

    Called as ``call RC.BAT <key>`` right after a case command, since anything
    else -- including writing a file -- would clobber ERRORLEVEL first.  Note the
    space before each ``>``: ``echo 0>FILE`` would parse as a redirect of handle
    0 rather than an echo of "0", so the value is written with a trailing space
    and stripped on the host.
    """
    lines = ["@echo off"]
    lines.extend(f"if errorlevel {value} goto R{value}" for value in _RC_LADDER)
    lines.extend(["echo 0 >RESULTS\\%1.RC", "goto END"])
    for value in _RC_LADDER:
        lines.extend([f":R{value}", f"echo {value} >RESULTS\\%1.RC", "goto END"])
    lines.extend([":END", ""])
    return "\r\n".join(lines)


def _batch(cases: list[Case], *, show_dos: bool, trace_dos: bool,
           prebuilt: bool = False) -> str:
    # The compiler build is the step that fails first and blocks everything after
    # it, so its output is captured exactly like a case command's.
    build = "call BUILD.BAT" if trace_dos else "call BUILD.BAT > RESULTS\\BUILD.LOG"
    if prebuilt:
        # FEC.EXE was restored from cache; BUILD.BAT would delete and rebuild it.
        # It also puts the Watcom binaries on PATH, which the case commands need
        # after it, so that line has to be reproduced rather than skipped.
        build = ("set PATH=%WATCOM%\\BINW;%WATCOM%\\BINP;%PATH%\r\n"
                 "echo OK>BUILD.OK")
    lines = [
        "@echo off", "if not exist RESULTS md RESULTS", "if not exist OUT md OUT",
        "set WATCOM=W:", "set INCLUDE=W:\\H",
        "set LIB=W:\\LIB286\\DOS;W:\\LIB286;W:\\LIB386\\DOS;W:\\LIB386",
        # COMMAND.COM can only redirect handle 1, so fec diagnostics written to
        # stderr never reach RESULTS\<key>.LOG.  Ask it for stdout instead.
        "set FE_DIAG_STDOUT=1",
        build, "if not exist BUILD.OK goto BUILDFAIL",
        "echo PASS>RESULTS\\BUILD.RES",
    ]
    for index, case in enumerate(cases):
        key = f"C{index:03d}"
        command = case.command
        # Watcom writes diagnostics into the current directory.  Isolate each
        # case so a later pytest item never sees stale diagnostics.
        lines.extend([
            "if exist *.ERR del *.ERR > NUL",
            f"if exist RESULTS\\{key}.ERR del RESULTS\\{key}.ERR > NUL",
        ])
        if not trace_dos:
            command += f" > RESULTS\\{key}.LOG"
        lines.extend([
            command,
            f"call RC.BAT {key}",
            f"if exist *.ERR type *.ERR > RESULTS\\{key}.ERR",
            f"if not exist RESULTS\\{key}.ERR type NUL > RESULTS\\{key}.ERR",
        ])
    lines.extend([
        "goto FINISH", ":BUILDFAIL", "echo FAIL>RESULTS\\BUILD.RES", ":FINISH",
        "echo DONE>RUN.OK",
        *(["pause"] if show_dos else []), "exit", "",
    ])
    return "\r\n".join(lines)


@dataclass
class SuiteRun:
    root: Path
    cases: list[Case]
    keep: bool = False

    @property
    def fec(self) -> Path:
        return self.root / "FEC"

    def _key(self, case: Case) -> str:
        return f"C{self.cases.index(case):03d}"

    def rc(self, case: Case) -> int | None:
        """Exit code the DOS command reported, or None if it was never recorded.

        Values above 8 are a lower bound; see ``_RC_LADDER``.
        """
        path = self.fec / "RESULTS" / f"{self._key(case)}.RC"
        if not path.is_file():
            return None
        text = path.read_text(encoding="ascii", errors="replace").strip()
        return int(text) if text.isdigit() else None

    def result(self, case: Case | None = None) -> str:
        if case is None:
            path = self.fec / "RESULTS" / "BUILD.RES"
            return path.read_text(encoding="ascii").strip() if path.is_file() else "MISSING"
        code = self.rc(case)
        if code is None:
            return "MISSING"
        return "PASS" if (code == 0) == case.expect_success else "FAIL"

    def log(self, case: Case | None = None) -> str:
        name = "BUILD" if case is None else self._key(case)
        path = self.fec / "RESULTS" / f"{name}.LOG"
        content = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        if content.strip():
            return content
        errors = sorted(self.fec.glob("*.ERR"))
        joined = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in errors)
        if joined.strip():
            return joined
        # Deliberately not falling back to CONSOLE.LOG: that is the emulator's own
        # log (display enumeration, INT15 chatter) and burying one useful line in
        # it reads as output when there was none.  Use --dos-log to see it.
        return "(no DOS output captured; the command wrote nothing before exiting)"

    def err(self, case: Case) -> str:
        path = self.fec / "RESULTS" / f"{self._key(case)}.ERR"
        return path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""

    def cleanup(self) -> None:
        if not self.keep:
            shutil.rmtree(self.root, ignore_errors=True)


def _compiler_key() -> str:
    """Hash of everything the compiler build reads.

    Sources and the build batch only; the toolchain itself is pinned by
    dosboxx.lock.json, so it cannot drift underneath a cache hit.
    """
    digest = hashlib.sha256()
    paths = sorted((ROOT / "fec" / "src").rglob("*"))
    paths.append(ROOT / "fec" / "build-dos.bat")
    for path in paths:
        if not path.is_file():
            continue
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def run_suite(cases: list[Case], *, keep: bool = False, show_dos: bool = False,
              trace_dos: bool = False) -> SuiteRun:
    dosbox, watcom = resolve_tools()
    RUNS.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="suite-", dir=RUNS))
    result = SuiteRun(run_root, cases, keep)
    fec = result.fec
    cached = CACHE / "compilers" / f"{_compiler_key()}.exe"
    try:
        shutil.copytree(ROOT / "fec" / "src", fec / "SRC")
        shutil.copytree(ROOT / "fec" / "std", fec / "STD")
        shutil.copytree(ROOT / "fec" / "tests", fec / "TESTS")
        shutil.copy2(ROOT / "fec" / "build-dos.bat", fec / "BUILD.BAT")
        console = run_root / "CONSOLE.LOG"
        config = run_root / "DOSBOX.CON"
        config.write_text(
            # Do not tune [cpu] here. core=dynamic is roughly 5x faster but the
            # recompiler loses abort()'s exit status -- a program that traps
            # exits 0 instead, so the M3 bounds cases stop reporting the trap
            # they exist to prove. Verified against a compiler built under
            # core=normal, so it is the runtime and not the build.
            f"[log]\nlogfile={console}\n"
            f"[dosbox]\nlog console=quiet\n",
            encoding="ascii",
        )
        if cached.is_file():
            shutil.copy2(cached, fec / "FEC.EXE")
        (fec / "RUN.BAT").write_text(
            _batch(cases, show_dos=show_dos, trace_dos=trace_dos,
                   prebuilt=cached.is_file()),
            encoding="ascii", newline="",
        )
        (fec / "RC.BAT").write_text(_rc_batch(), encoding="ascii", newline="")
        command = [str(dosbox)]
        if not show_dos:
            command.append("-silent")
        command.extend([
            "-fastlaunch", "-conf", str(config),
            "-c", f'mount C "{run_root}"', "-c", f'mount R "{ROOT}" -ro',
            "-c", f'mount W "{watcom}" -ro',
            "-c", "C:", "-c", "cd \\FEC", "-c", "RUN.BAT",
        ])
        # Every case pays a DOS process spawn, and the compile-only checks spawn
        # wcc386 once each, so the whole-suite run is minutes rather than the
        # under-a-minute a single milestone takes.
        completed = subprocess.run(command, check=False, timeout=1800)
        if completed.returncode != 0:
            raise DosboxError(f"DOSBox-X exited with status {completed.returncode}")
        if not (fec / "RUN.OK").is_file():
            raise DosboxError("DOSBox-X did not complete the test batch")
        built = fec / "FEC.EXE"
        if not cached.is_file() and built.is_file() and result.result() == "PASS":
            cached.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(built, cached)
        return result
    except Exception:
        result.keep = True
        raise
