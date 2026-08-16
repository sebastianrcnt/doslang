from __future__ import annotations

import os
import warnings

import pytest

from ferrolang_vm.dosboxx import SuiteRun, run_suite
from ferrolang_vm.suite import Case, all_cases


def _number(name: str) -> int:
    if not name.startswith("m") or not name[1:].isdigit():
        raise ValueError(f"invalid milestone: {name}")
    value = int(name[1:])
    if value not in range(1, 7):
        raise ValueError(f"unsupported milestone: {name}")
    return value


ONLY = os.environ.get("FERRO_TEST_ONLY")
CASES = all_cases(
    through=_number(os.environ.get("FERRO_TEST_THROUGH", "m6")),
    only=_number(ONLY) if ONLY else None,
)


@pytest.fixture(scope="session")
def suite_run() -> SuiteRun:
    run = run_suite(
        CASES,
        keep=os.environ.get("FERRO_TEST_KEEP_FAILED") == "1",
        show_dos=os.environ.get("FERRO_TEST_SHOW_DOS") == "1",
        trace_dos=os.environ.get("FERRO_TEST_TRACE_DOS") == "1",
    )
    yield run
    if os.environ.get("FERRO_TEST_DOS_LOG") == "1":
        console = run.root / "CONSOLE.LOG"
        if console.is_file():
            print(console.read_text(encoding="utf-8", errors="replace"))
    run.cleanup()


def test_compiler_build(suite_run: SuiteRun) -> None:
    assert suite_run.result() == "PASS", suite_run.log()


@pytest.mark.parametrize("case", CASES, ids=lambda case: case.id)
def test_milestone_case(case: Case, suite_run: SuiteRun) -> None:
    if suite_run.result() != "PASS":
        pytest.skip("compiler build failed")
    result = suite_run.result(case)
    err = suite_run.err(case)
    if result == "PASS" and err:
        warning_lines = [line for line in err.splitlines() if "warning" in line.lower()]
        if warning_lines:
            warnings.warn("\n".join(warning_lines), stacklevel=1)
    assert result == "PASS", (
        f"DOS command: {case.command}\nExpected success: {case.expect_success}\n"
        f"{suite_run.log(case)}\n{err}"
    )
