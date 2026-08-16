"""General-purpose disposable DOSBox-X/Open Watcom environment CLI."""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from .dosboxx import DosboxError, run_suite, setup
from .paths import ROOT
from .suite import Case


def _run(command: str | None, *, keep: bool, show_dos: bool) -> int:
    cases = [] if command is None else [Case("command", 0, command, True)]
    run = run_suite(cases, keep=keep, show_dos=show_dos, trace_dos=show_dos)
    try:
        if run.result() != "PASS":
            print(run.log(), file=sys.stderr)
            return 1
        if cases and run.result(cases[0]) != "PASS":
            print(run.log(cases[0]), file=sys.stderr)
            return 1
        if cases:
            output = run.log(cases[0])
            if output:
                print(output, end="" if output.endswith("\n") else "\n")
        if keep:
            print(f"DOS workspace: {run.root}")
        return 0
    finally:
        run.cleanup()


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="ferro-dos",
        description="Disposable directory-backed DOSBox-X/Open Watcom environment.",
    )
    commands = parser.add_subparsers(dest="action", required=True)
    prepare = commands.add_parser("setup", help="install the pinned DOSBox-X and Open Watcom tools")
    prepare.add_argument("--accept-watcom-license", action="store_true")
    for name, help_text in (
        ("build", "build the current FEC source inside DOS"),
        ("exec", "build FEC and execute one DOS command"),
        ("batch", "build FEC and call a repository DOS batch"),
        ("shell", "build FEC and open an interactive DOS shell"),
    ):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--keep", action="store_true", help="preserve the temporary DOS workspace")
        command.add_argument("--show-dos", action="store_true", help="show and pause the DOS window")
        if name == "exec":
            command.add_argument("dos_command")
        elif name == "batch":
            command.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "setup":
            dosbox, watcom = setup(accept_watcom_license=args.accept_watcom_license)
            print(f"DOSBox-X: {dosbox}")
            print(f"Open Watcom: {watcom}")
            return 0
        if args.action == "build":
            return _run(None, keep=args.keep, show_dos=args.show_dos)
        if args.action == "exec":
            return _run(args.dos_command, keep=args.keep, show_dos=args.show_dos)
        if args.action == "shell":
            return _run("COMMAND.COM", keep=args.keep, show_dos=True)
        path = (ROOT / args.path).resolve()
        if (path != ROOT and ROOT not in path.parents) or not path.is_file():
            raise DosboxError("batch path must be an existing file inside the repository")
        relative = path.relative_to(ROOT).as_posix().replace("/", "\\").upper()
        return _run(f"CALL R:\\{relative}", keep=args.keep, show_dos=args.show_dos)
    except (DosboxError, subprocess.SubprocessError) as exc:
        print(f"ferro-dos: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
