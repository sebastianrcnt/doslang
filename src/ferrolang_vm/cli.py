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


def main() -> int:
    parser = argparse.ArgumentParser(description="Windows-only QEMU/FreeDOS automation")
    commands = parser.add_subparsers(dest="op", required=True)
    for name in ("start", "stop", "status", "ping", "screenshot", "ocr", "logs"):
        commands.add_parser(name)
    wait = commands.add_parser("wait-ready")
    wait.add_argument("--timeout", type=int, default=45)
    reset = commands.add_parser("reset")
    reset.add_argument("--timeout", type=int, default=45)
    execute = commands.add_parser("exec")
    execute.add_argument("command")
    put = commands.add_parser("put")
    put.add_argument("source", type=Path)
    put.add_argument("destination")
    get = commands.add_parser("get")
    get.add_argument("source")
    get.add_argument("destination", type=Path)
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
