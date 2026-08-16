"""Host-side checks for constraints the DOS toolchain enforces far too late.

Everything here runs without DOSBox-X.  The point is to fail in a tenth of a
second with the offending name, instead of after a DOSBox-X boot and ten
successful object builds -- and with a message that says what is actually wrong.
A 9-character source name reaches the DOS build as ``Unable to open "src\\x.c"``,
which reads as a missing file rather than a name that cannot be represented.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from ferrolang_vm.paths import ROOT
from ferrolang_vm.registry import CASES

# The runner copies these onto a FAT filesystem, where a name is at most eight
# characters plus a three-character extension.
COPIED_TREES = ("fec/src", "fec/std", "fec/tests")


def _offenders(root: Path) -> list[str]:
    bad = []
    for path in sorted(root.rglob("*")):
        name = path.name
        if name.startswith("."):
            continue
        stem, _, suffix = name.rpartition(".") if "." in name else (name, "", "")
        if len(stem) > 8 or len(suffix) > 3:
            bad.append(f"{path.relative_to(ROOT).as_posix()} (stem {len(stem)}, ext {len(suffix)})")
    return bad


@pytest.mark.parametrize("tree", COPIED_TREES)
def test_copied_files_fit_dos_8_3(tree: str) -> None:
    root = ROOT / tree
    if not root.is_dir():
        pytest.skip(f"{tree} is absent")
    bad = _offenders(root)
    assert not bad, (
        f"{len(bad)} name(s) under {tree} cannot be represented on the DOS side.\n"
        "The DOS build will report them as missing files, not as long names:\n  "
        + "\n  ".join(bad)
    )


def test_registry_paths_exist_on_the_host() -> None:
    """Every ``.FE`` a case names must exist, matched case-insensitively.

    DOS is case-insensitive, so a registry typo survives until the command runs
    inside the VM and fails with a message about the wrong thing.
    """
    available = {
        path.relative_to(ROOT / "fec").as_posix().upper()
        for path in (ROOT / "fec").rglob("*.fe")
    }
    available |= {
        path.relative_to(ROOT / "fec").as_posix().upper()
        for path in (ROOT / "fec").rglob("*.FE")
    }
    missing = []
    for case in CASES:
        for token in case.command.split():
            if not token.upper().endswith(".FE"):
                continue
            wanted = token.replace("\\", "/").upper()
            if wanted.startswith("STD/"):
                wanted = f"STD/{wanted[4:]}"
            if not any(entry.endswith(wanted) for entry in available):
                missing.append(f"{case.id}: {token}")
    assert not missing, "registry names fixtures that do not exist:\n  " + "\n  ".join(missing)
