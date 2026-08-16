"""Developer test entry point backed by a disposable DOSBox-X run."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

from .dosboxx import DosboxError, setup
from .registry import MAX_MILESTONE, MILESTONES


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
    selection.add_argument("--through", choices=MILESTONES, default=f"m{MAX_MILESTONE}",
                           help="run cumulatively through this milestone "
                                f"(default: m{MAX_MILESTONE})")
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
    run.add_argument("-k", dest="select", metavar="EXPR",
                     help="run only cases whose id matches this pytest -k expression")
    args, extra = parser.parse_known_args()
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
        from .paths import ROOT
        # The package can be imported from a different checkout than the one the
        # shell is sitting in -- an editable install plus a git worktree is enough
        # to silently build and test the wrong tree.  Say which tree this is.
        print(f"ferro-test: building {ROOT}", file=sys.stderr)
        test_file = os.fspath(ROOT / "tools" / "tests" / "test_milestones_dosboxx.py")
        pytest_args = [test_file, "--tb=short", "-v" if args.verbose else "-q"]
        if args.select:
            pytest_args.extend(["-k", args.select])
        if args.dos_log:
            pytest_args.append("-s")
        pytest_args.extend(extra)
        return int(pytest.main(pytest_args))
    except (DosboxError, ValueError) as exc:
        print(f"ferro-test: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
