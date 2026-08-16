"""Developer test entry point backed by a disposable DOSBox-X run."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from .dosboxx import DosboxError, setup


MILESTONES = tuple(f"m{number}" for number in range(1, 7))


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="ferro-test",
        description="Fast Ferro development tests in DOSBox-X/Open Watcom.",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("setup", help="download and verify pinned development tools")
    prepare.add_argument("--accept-watcom-license", action="store_true",
                         help="confirm acceptance of the Sybase Open Watcom Public License")
    run = commands.add_parser("run", help="build once and run milestone pytest cases")
    selection = run.add_mutually_exclusive_group()
    selection.add_argument("--through", choices=MILESTONES, default="m6",
                           help="run cumulatively through this milestone (default: m6)")
    selection.add_argument("--only", choices=MILESTONES,
                           help="run only this milestone's cases")
    run.add_argument("-v", "--verbose", action="store_true", help="show every pytest case")
    run.add_argument("--keep-failed", action="store_true",
                     help="keep the disposable DOS filesystem after failures")
    run.add_argument("--show-dos", action="store_true",
                     help="show DOSBox-X and wait for a key before closing")
    run.add_argument("--dos-log", action="store_true",
                     help="print the captured DOS console after the run")
    run.add_argument("--trace-dos", action="store_true",
                     help="do not redirect case command output")
    args = parser.parse_args()
    try:
        if args.command == "setup":
            dosbox, watcom = setup(accept_watcom_license=args.accept_watcom_license)
            print(f"DOSBox-X: {dosbox}")
            print(f"Open Watcom: {watcom}")
            return 0
        if args.only:
            os.environ["FERRO_TEST_ONLY"] = args.only
        else:
            os.environ["FERRO_TEST_THROUGH"] = args.through
        for enabled, name in (
            (args.keep_failed, "FERRO_TEST_KEEP_FAILED"),
            (args.show_dos, "FERRO_TEST_SHOW_DOS"),
            (args.trace_dos, "FERRO_TEST_TRACE_DOS"),
            (args.dos_log, "FERRO_TEST_DOS_LOG"),
        ):
            if enabled:
                os.environ[name] = "1"
        import pytest
        test_file = os.fspath(
            Path(__file__).resolve().parents[2] / "tools" / "tests" / "test_milestones_dosboxx.py"
        )
        pytest_args = [test_file, "--tb=short", "-v" if args.verbose else "-q"]
        if args.dos_log:
            pytest_args.append("-s")
        return int(pytest.main(pytest_args))
    except (DosboxError, ValueError) as exc:
        print(f"ferro-test: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
