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

from .daemon import ROOT
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
    marker = CACHE / "SETUP.OK"
    lock_hash = _sha256(LOCK_PATH)
    if (marker.is_file() and marker.read_text(encoding="ascii").strip() == lock_hash
            and dosbox.is_file() and all(path.is_file() for path in watcom_required)):
        return dosbox, watcom
    if not accept_watcom_license:
        raise DosboxError(
            "Open Watcom is distributed under the Sybase Open Watcom Public License. "
            "Review tools/toolchains/dosboxx.lock.json and rerun "
            "`uv run ferro-test setup --accept-watcom-license`."
        )
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
    marker.write_text(lock_hash + "\n", encoding="ascii")
    return dosbox, watcom


def resolve_tools() -> tuple[Path, Path]:
    lock = _lock()
    dosbox, watcom, required = _required_paths(lock)
    if not dosbox.is_file() or not all(path.is_file() for path in required):
        raise DosboxError("toolchain is not installed; run `uv run ferro-test setup`")
    return dosbox, watcom


def _batch(cases: list[Case], *, show_dos: bool, trace_dos: bool) -> str:
    lines = [
        "@echo off", "if not exist RESULTS md RESULTS", "if not exist OUT md OUT",
        "set WATCOM=W:", "set INCLUDE=W:\\H",
        "set LIB=W:\\LIB286\\DOS;W:\\LIB286;W:\\LIB386\\DOS;W:\\LIB386",
        "call BUILD.BAT", "if not exist BUILD.OK goto BUILDFAIL",
        "echo PASS>RESULTS\\BUILD.RES",
    ]
    for index, case in enumerate(cases):
        key = f"C{index:03d}"
        command = case.command
        if not trace_dos:
            command += f" > RESULTS\\{key}.LOG"
        lines.append(command)
        if case.expect_success:
            lines.extend([
                f"if errorlevel 1 goto {key}F", f"echo PASS>RESULTS\\{key}.RES",
                f"goto {key}D", f":{key}F", f"echo FAIL>RESULTS\\{key}.RES", f":{key}D",
            ])
        else:
            lines.extend([
                f"if errorlevel 1 goto {key}P", f"echo FAIL>RESULTS\\{key}.RES",
                f"goto {key}D", f":{key}P", f"echo PASS>RESULTS\\{key}.RES", f":{key}D",
            ])
    lines.extend([
        "goto FINISH", ":BUILDFAIL", "echo FAIL>RESULTS\\BUILD.RES", ":FINISH",
        "echo DONE>RUN.OK", *(["pause"] if show_dos else []), "exit", "",
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

    def result(self, case: Case | None = None) -> str:
        name = "BUILD" if case is None else self._key(case)
        path = self.fec / "RESULTS" / f"{name}.RES"
        return path.read_text(encoding="ascii").strip() if path.is_file() else "MISSING"

    def log(self, case: Case | None = None) -> str:
        name = "BUILD" if case is None else self._key(case)
        path = self.fec / "RESULTS" / f"{name}.LOG"
        content = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
        if content:
            return content
        errors = sorted(self.fec.glob("*.ERR"))
        if errors:
            return "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in errors)
        console = self.root / "CONSOLE.LOG"
        return console.read_text(encoding="utf-8", errors="replace") if console.is_file() else ""

    def cleanup(self) -> None:
        if not self.keep:
            shutil.rmtree(self.root, ignore_errors=True)


def run_suite(cases: list[Case], *, keep: bool = False, show_dos: bool = False,
              trace_dos: bool = False) -> SuiteRun:
    dosbox, watcom = resolve_tools()
    RUNS.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="suite-", dir=RUNS))
    result = SuiteRun(run_root, cases, keep)
    fec = result.fec
    try:
        shutil.copytree(ROOT / "fec" / "src", fec / "SRC")
        shutil.copytree(ROOT / "fec" / "std", fec / "STD")
        shutil.copytree(ROOT / "fec" / "tests", fec / "TESTS")
        shutil.copy2(ROOT / "fec" / "build-dos.bat", fec / "BUILD.BAT")
        console = run_root / "CONSOLE.LOG"
        config = run_root / "DOSBOX.CON"
        config.write_text(
            f"[log]\nlogfile={console}\n[dosbox]\nlog console=quiet\n",
            encoding="ascii",
        )
        (fec / "RUN.BAT").write_text(
            _batch(cases, show_dos=show_dos, trace_dos=trace_dos),
            encoding="ascii", newline="",
        )
        command = [str(dosbox)]
        if not show_dos:
            command.append("-silent")
        command.extend([
            "-fastlaunch", "-conf", str(config),
            "-c", f'mount C "{run_root}"', "-c", f'mount W "{watcom}" -ro',
            "-c", "C:", "-c", "cd \\FEC", "-c", "RUN.BAT",
        ])
        completed = subprocess.run(command, check=False, timeout=300)
        if completed.returncode != 0:
            raise DosboxError(f"DOSBox-X exited with status {completed.returncode}")
        if not (fec / "RUN.OK").is_file():
            raise DosboxError("DOSBox-X did not complete the test batch")
        return result
    except Exception:
        result.keep = True
        raise
