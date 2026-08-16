from __future__ import annotations

import os
import warnings

import pytest

from ferrolang_vm.dosboxx import SuiteRun, run_suite
from ferrolang_vm.registry import MAX_MILESTONE, all_cases, milestone_number
from ferrolang_vm.suite import Case

ONLY = os.environ.get("FERRO_TEST_ONLY")
CASES = all_cases(
    through=milestone_number(os.environ.get("FERRO_TEST_THROUGH", f"m{MAX_MILESTONE}")),
    only=milestone_number(ONLY) if ONLY else None,
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
    code = suite_run.rc(case)
    assert result == "PASS", (
        f"DOS command: {case.command}\n"
        f"Expected success: {case.expect_success}\n"
        f"Exit code: {'not recorded' if code is None else code}\n"
        f"{suite_run.log(case)}\n{err}"
    )
