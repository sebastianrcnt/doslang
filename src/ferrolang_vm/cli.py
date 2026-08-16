"""Command line client for the Windows-only ferro-vm daemon."""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from multiprocessing.connection import Client
from pathlib import Path

from .daemon import PIPE, ROOT


def rpc(payload: dict[str, object], start_daemon: bool = False) -> object:
    try:
        conn = Client(PIPE, family="AF_PIPE")
    except (FileNotFoundError, OSError):
        if not start_daemon:
            raise RuntimeError("ferro-vm daemon is not running; run `uv run ferro-vm start`")
        flags = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
        subprocess.Popen([sys.executable, "-m", "ferrolang_vm.daemon"], cwd=ROOT, creationflags=flags, close_fds=True)
        deadline = time.monotonic() + 5
        while True:
            try:
                conn = Client(PIPE, family="AF_PIPE")
                break
            except (FileNotFoundError, OSError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("ferro-vm daemon did not create its control pipe")
                time.sleep(.1)
    with conn:
        conn.send(payload)
        response = conn.recv()
    if not response["ok"]:
        raise RuntimeError(response["error"])
    return response["result"]


def wait_ready(timeout: int) -> bool:
    """Wait quietly; do not turn an expected boot gap into error-log spam."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            status = rpc({"op": "status"})
            if status["agent_connected"] and str(rpc({"op": "ping"})["response"]).startswith("OK 504F4E47"):
                return True
        except RuntimeError:
            pass
        time.sleep(.5)
    return False


def follow_logs() -> int:
    log_path = ROOT / ".qemu" / "ferro-vm.log"
    lnav = shutil.which("lnav.exe") or shutil.which("lnav")
    if lnav:
        return subprocess.run([lnav, str(log_path)]).returncode
    print("lnav was not found; following the log with PowerShell.", file=sys.stderr)
    return subprocess.run([
        "powershell", "-NoProfile", "-Command",
        f"Get-Content -LiteralPath '{log_path}' -Wait",
    ]).returncode


EPILOG = r"""examples:
  uv run ferro-vm start                  boot the VM and start the daemon
  uv run ferro-vm wait-ready             block until TCPAGENT answers PING
  uv run ferro-vm exec 'dir C:\FEC'      run a DOS command, print exit code and output
  uv run ferro-vm put fec/src/check.c 'C:\FEC\SRC\CHECK.C'
  uv run ferro-vm get 'C:\FEC\TEST.OK' .qemu/TEST.OK
  uv run ferro-vm logs                   follow the structured daemon log

The authoritative workspace is C:\FEC inside the VM. Never build on D: (the vvfat
view is for exchange only). See AGENTS.md for verification rules and build traps.
"""

SIMPLE_COMMANDS = {
    "start": "Start the daemon and boot QEMU. Safe to run when already up.",
    "stop": "Quit QEMU cleanly and stop the daemon.",
    "status": "Print daemon, QEMU, and TCPAGENT connection state as JSON.",
    "ping": "Send PING to TCPAGENT. Expects 'OK 504F4E47' (PONG).",
    "screenshot": "Capture the VGA console to a PPM/PNG under .qemu/.",
    "ocr": "Capture the console and print recognized text (RapidOCR).",
    "logs": "Follow the append-only daemon log. Uses lnav when available.",
}


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="ferro-vm",
        description="Windows-only QEMU/FreeDOS automation for the Ferro compiler.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    commands = parser.add_subparsers(dest="op", required=True, metavar="COMMAND")
    for name, blurb in SIMPLE_COMMANDS.items():
        commands.add_parser(name, help=blurb, description=blurb)

    wait_help = "Block until TCPAGENT is connected and answers PING."
    wait = commands.add_parser("wait-ready", help=wait_help, description=wait_help)
    wait.add_argument("--timeout", type=int, default=45, metavar="SECONDS",
                      help="give up after this many seconds (default: %(default)s)")

    reset_help = "Quit QEMU cleanly, reboot it, and wait for TCPAGENT."
    reset = commands.add_parser("reset", help=reset_help, description=reset_help)
    reset.add_argument("--timeout", type=int, default=45, metavar="SECONDS",
                       help="give up after this many seconds (default: %(default)s)")

    exec_help = "Run a DOS command inside the VM and print its exit code and output."
    execute = commands.add_parser(
        "exec", help=exec_help,
        description=exec_help + " Quote the command so the host shell does not eat"
                                r" backslashes: exec 'wcl386 -q HELLO.C'.")
    execute.add_argument("command", metavar="DOS_COMMAND",
                         help=r"command line to hand to COMMAND.COM, e.g. 'dir C:\FEC'")

    put_help = "Copy a host file into the VM."
    put = commands.add_parser("put", help=put_help, description=put_help)
    put.add_argument("source", type=Path, metavar="HOST_PATH",
                     help="file on this machine")
    put.add_argument("destination", metavar="DOS_PATH",
                     help=r"target inside the VM, e.g. 'C:\FEC\SRC\CHECK.C'."
                          " DOS uses 8.3 names, so long fixtures must be shortened"
                          " explicitly (BAD-ARI.FE, TRY-FPR.FE)")

    get_help = "Copy a file out of the VM onto the host."
    get = commands.add_parser("get", help=get_help, description=get_help)
    get.add_argument("source", metavar="DOS_PATH",
                     help=r"file inside the VM, e.g. 'C:\FEC\TEST.OK'")
    get.add_argument("destination", type=Path, metavar="HOST_PATH",
                     help="target on this machine")

    args = parser.parse_args()

    try:
        if args.op == "logs":
            return follow_logs()
        if args.op == "wait-ready":
            if wait_ready(args.timeout):
                print(json.dumps({"agent": "PONG"}))
                return 0
            raise RuntimeError("TCPAGENT did not become ready")
        if args.op == "ocr":
            result = rpc({"op": "screenshot"})
            import logging
            logging.disable(logging.INFO)
            from rapidocr import RapidOCR
            recognized = RapidOCR()(result["path"])
            print("\n".join(recognized.txts or ()))
            return 0
        if args.op == "reset":
            rpc({"op": "stop"}, start_daemon=True)
            time.sleep(.5)
            rpc({"op": "start"}, start_daemon=True)
            time.sleep(2)
            rpc({"op": "monitor", "command": "sendkey ret"})
            if wait_ready(args.timeout):
                print(json.dumps({"reset": "complete", "agent": "PONG"}))
                return 0
            raise RuntimeError("TCPAGENT did not become ready")
        payload: dict[str, object] = {"op": args.op}
        if args.op == "exec": payload["command"] = args.command
        if args.op == "put":
            payload["source"] = str(args.source.resolve())
            payload["destination"] = args.destination
        if args.op == "get":
            payload["source"] = args.source
            payload["destination"] = str(args.destination.resolve())
        print(json.dumps(rpc(payload, start_daemon=args.op == "start"), ensure_ascii=False, indent=2))
        return 0
    except RuntimeError as exc:
        print(f"ferro-vm: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
