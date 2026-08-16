"""Long-lived Windows host for the FreeDOS TCP agent.

The only automation TCP listener is 127.0.0.1:5558, used exclusively by
TCPAGENT.EXE. Local commands use a Windows named pipe.
"""
from __future__ import annotations

import json
import logging
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from multiprocessing.connection import Listener
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QEMU = ROOT / ".qemu"
PIPE = r"\\.\pipe\ferrolang-vm"
AGENT_ADDRESS = ("127.0.0.1", 5558)
MONITOR_ADDRESS = ("127.0.0.1", 4444)
LOG_PATH = QEMU / "ferro-vm.log"

# Short commands answer promptly, so a plain socket timeout is the right guard.
REQUEST_TIMEOUT = 30
# EXEC is different: TCPAGENT is frozen inside system() for the whole command
# and cannot answer, so silence proves nothing. Wake up often, decide with
# guest liveness instead of a stopwatch, and never discard the connection just
# because a compile is slow.
EXEC_POLL_SECONDS = 2
DEFAULT_IDLE_TIMEOUT = 60
DEFAULT_HARD_TIMEOUT = 900
# Budget for collecting the result after Ctrl+C, before giving up on the stream.
INTERRUPT_GRACE_SECONDS = 15


class ExecInterrupted(RuntimeError):
    """The supervisor decided to stop the running DOS command."""


def configure_logging() -> None:
    QEMU.mkdir(exist_ok=True)
    handler = logging.FileHandler(LOG_PATH, encoding="utf-8")
    handler.setFormatter(logging.Formatter("%(asctime)s.%(msecs)03dZ %(levelname)-7s %(message)s", "%Y-%m-%dT%H:%M:%S"))
    logging.basicConfig(level=logging.INFO, handlers=[handler])
    logging.Formatter.converter = time.gmtime


def log_event(level: int, event: str, **fields: object) -> None:
    suffix = " ".join(f"{key}={json.dumps(value, ensure_ascii=False)}" for key, value in fields.items())
    logging.log(level, "%s%s", event, f" {suffix}" if suffix else "")


class Host:
    def __init__(self) -> None:
        self.agent: socket.socket | None = None
        self.agent_lock = threading.Lock()
        self.agent_ready = threading.Event()
        self.qemu: subprocess.Popen[bytes] | None = None
        # QEMU accepts one monitor connection at a time, and the EXEC
        # supervisor polls it while other control requests run concurrently.
        self.monitor_lock = threading.Lock()
        self.abort_requested = threading.Event()
        self.exec_active = threading.Event()

    @staticmethod
    def bind_agent_listener() -> socket.socket:
        """Bind 5558 exclusively so a second daemon fails loudly.

        SO_REUSEADDR means something different on Windows than on Unix: it lets
        another process bind an already-bound port and quietly take over new
        connections, so a duplicate daemon would be silently half-working
        instead of refusing to start. SO_EXCLUSIVEADDRUSE is the Windows way to
        say "only me".
        """
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        exclusive = getattr(socket, "SO_EXCLUSIVEADDRUSE", None)
        if exclusive is not None:
            server.setsockopt(socket.SOL_SOCKET, exclusive, 1)
        else:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(AGENT_ADDRESS)
        server.listen(1)
        return server

    def accept_agents(self, server: socket.socket) -> None:
        log_event(logging.INFO, "agent listener ready", address="127.0.0.1:5558")
        while True:
            sock, peer = server.accept()
            sock.settimeout(REQUEST_TIMEOUT)
            with self.agent_lock:
                if self.agent is not None:
                    sock.close()
                    log_event(logging.WARNING, "agent rejected", peer=str(peer), reason="already connected")
                    continue
                self.agent = sock
            try:
                banner = self._read_line(sock).decode("ascii", "replace")
                if banner != "TCPAGENT READY":
                    raise ConnectionError(f"invalid TCPAGENT banner: {banner!r}")
                self.agent_ready.set()
                log_event(logging.INFO, "agent connected", peer=f"{peer[0]}:{peer[1]}")
                # Request() owns protocol reads. Between requests, peek only
                # for EOF so TCPAGENT can reconnect without being rejected.
                while self.agent is sock:
                    if self.agent_lock.acquire(blocking=False):
                        try:
                            readable, _, _ = select.select([sock], [], [], .25)
                            if readable and not sock.recv(1, socket.MSG_PEEK):
                                break
                        finally:
                            self.agent_lock.release()
                    else:
                        time.sleep(.05)
            except (ConnectionError, OSError) as exc:
                # Reset can close a connection during its banner. This is a
                # per-connection event, never a reason to kill the listener.
                log_event(logging.WARNING, "agent handshake/connection failed",
                          peer=f"{peer[0]}:{peer[1]}", error=str(exc))
            finally:
                with self.agent_lock:
                    if self.agent is sock:
                        self.agent = None
                        self.agent_ready.clear()
                sock.close()
                log_event(logging.INFO, "agent disconnected", peer=f"{peer[0]}:{peer[1]}")

    @staticmethod
    def _read_line(sock: socket.socket, supervise=None) -> bytes:
        # Partial input is kept across timeouts, so supervise() may fire in the
        # middle of a line without losing what has already arrived.
        out = bytearray()
        while not out.endswith(b"\n"):
            try:
                part = sock.recv(1)
            except TimeoutError:
                if supervise is None:
                    raise
                supervise()
                continue
            if not part:
                raise ConnectionError("TCP agent closed connection")
            out.extend(part)
        return bytes(out).rstrip(b"\r\n")

    @staticmethod
    def _read_exactly(sock: socket.socket, count: int, supervise=None) -> bytes:
        chunks: list[bytes] = []
        while count:
            try:
                chunk = sock.recv(min(65536, count))
            except TimeoutError:
                if supervise is None:
                    raise
                supervise()
                continue
            if not chunk:
                raise ConnectionError("TCP agent closed connection")
            chunks.append(chunk)
            count -= len(chunk)
        return b"".join(chunks)

    def guest_idle_seconds(self) -> float | None:
        """Seconds since the guest last touched a disk, or None if unknown.

        QEMU keeps counting while TCPAGENT is frozen inside system(), so this
        is the one progress signal available during a long DOS command. A
        purely CPU-bound command looks idle here, which is what the hard
        timeout is for.
        """
        try:
            text = self.monitor("info blockstats")
        except OSError:
            return None
        idle: float | None = None
        for line in text.splitlines():
            operations = re.search(r"rd_operations=(\d+)", line)
            elapsed = re.search(r"idle_time_ns=(\d+)", line)
            if not operations or not elapsed or int(operations.group(1)) == 0:
                continue
            seconds = int(elapsed.group(1)) / 1e9
            idle = seconds if idle is None else min(idle, seconds)
        return idle

    def request(self, command: str, payload: bytes = b"") -> str:
        with self.agent_lock:
            if self.agent is None:
                raise RuntimeError("TCPAGENT is not connected")
            sock = self.agent
            started = time.monotonic()
            try:
                sock.sendall(command.encode("ascii") + b"\n" + payload)
                response = self._read_line(sock).decode("ascii", "replace")
            except OSError as exc:
                if self.agent is sock:
                    self.agent = None
                    self.agent_ready.clear()
                raise RuntimeError(f"TCPAGENT request failed: {exc}") from exc
        log_event(logging.INFO, "agent request", command=command.split(" ", 1)[0], response=response[:200], elapsed_ms=round((time.monotonic()-started)*1000))
        if response.startswith("ERR "):
            raise RuntimeError(bytes.fromhex(response[4:]).decode("utf-8", "replace"))
        return response

    def ping(self) -> dict[str, object]:
        return {"response": self.request("PING")}

    def _read_exec_result(self, sock: socket.socket, supervise=None) -> tuple[int, int, bytes]:
        header = self._read_line(sock, supervise).decode("ascii", "replace")
        fields = header.split()
        if len(fields) == 4 and fields[0] == "RESULT":
            code, length, flags = int(fields[1]), int(fields[2]), int(fields[3])
            return code, flags, self._read_exactly(sock, length, supervise)
        if fields and fields[0] == "OK":
            # Compatibility with an installed pre-RESULT agent.
            return int(fields[1]), 1, bytes.fromhex(fields[2]) if len(fields) > 2 else b""
        raise RuntimeError("malformed EXEC response: " + header)

    def exec(self, command: str, idle_timeout: float = DEFAULT_IDLE_TIMEOUT,
             hard_timeout: float = DEFAULT_HARD_TIMEOUT) -> dict[str, object]:
        log_event(logging.INFO, "exec start", command=command,
                  idle_timeout=idle_timeout, hard_timeout=hard_timeout)
        started = time.monotonic()
        self.abort_requested.clear()
        with self.agent_lock:
            if self.agent is None:
                raise RuntimeError("TCPAGENT is not connected")
            sock = self.agent
            self.exec_active.set()
            reported = started
            interrupt_at: float | None = None
            interrupted = ""

            def supervise() -> None:
                """Called every EXEC_POLL_SECONDS while the agent stays silent."""
                nonlocal reported, interrupt_at, interrupted
                now = time.monotonic()
                idle = self.guest_idle_seconds()
                if now - reported >= 15:
                    reported = now
                    log_event(logging.INFO, "exec running", elapsed_s=round(now-started, 1),
                              guest_idle_s=None if idle is None else round(idle, 1))
                if interrupt_at is None:
                    if self.abort_requested.is_set():
                        interrupted = "aborted by request"
                    elif now - started > hard_timeout:
                        interrupted = f"hard timeout after {hard_timeout:.0f}s"
                    elif idle is not None and idle > idle_timeout:
                        interrupted = f"guest idle {idle:.0f}s exceeds {idle_timeout:.0f}s"
                    if interrupted:
                        interrupt_at = now
                        log_event(logging.WARNING, "exec interrupting", reason=interrupted)
                        self.monitor("sendkey ctrl-c")
                    return
                waited = now - interrupt_at
                # DOS answers Ctrl+C with "Terminate batch file (Y/N/A)?" and
                # waits there. The prompt only appears once COMMAND.COM reaches
                # the next batch line, which can be many seconds into a slow
                # command, so answer on every poll rather than once: a single
                # early 'y' is swallowed by whatever is still running. Send only
                # 'y' -- the prompt takes one keystroke, and a trailing Enter
                # gets read as "keep going".
                self.monitor("sendkey y")
                # Even answered, Ctrl+C is a request. It lands only at a DOS
                # break check, and a DOS/4GW child (wcc386, wmake) runs in
                # protected mode where it may never reach one, so the command
                # can still run to completion. Keep collecting its result rather
                # than abandoning a stream that still owes us one -- give up
                # only once the guest has gone quiet too.
                if waited > INTERRUPT_GRACE_SECONDS and (idle is None or idle > 5):
                    raise ExecInterrupted(interrupted + "; command did not stop")

            try:
                encoded = command.encode("ascii", "replace").hex().upper()
                sock.settimeout(EXEC_POLL_SECONDS)
                sock.sendall(f"EXEC {encoded}\n".encode("ascii"))
                code, flags, raw = self._read_exec_result(sock, supervise)
            except (OSError, ExecInterrupted) as exc:
                # Only now is the stream beyond repair; drop it so the agent
                # reconnects with a clean protocol state.
                if self.agent is sock:
                    self.agent = None
                    self.agent_ready.clear()
                sock.close()
                raise RuntimeError(f"TCPAGENT EXEC failed: {exc}") from exc
            finally:
                self.exec_active.clear()
                self.abort_requested.clear()
                try:
                    sock.settimeout(REQUEST_TIMEOUT)
                except OSError:
                    pass
        output = raw.decode("cp437", "replace")
        for line in output.splitlines():
            log_event(logging.INFO, "dos output", line=line)
        log_event(logging.INFO, "exec finish", exit=code, bytes=len(raw), flags=flags,
                  interrupted=interrupted or None,
                  elapsed_ms=round((time.monotonic()-started)*1000))
        result = {"exit": code, "output": output, "bytes": len(raw), "flags": flags}
        if interrupted:
            result["interrupted"] = interrupted
        return result

    def abort(self) -> dict[str, object]:
        if not self.exec_active.is_set():
            return {"aborted": False, "reason": "no command is running"}
        self.abort_requested.set()
        log_event(logging.INFO, "abort requested")
        return {"aborted": True}

    def put(self, source: str, destination: str) -> dict[str, object]:
        data = Path(source).read_bytes()
        encoded = destination.encode("ascii").hex().upper()
        self.request(f"PUT {encoded} {len(data)}", data)
        stat = self.request(f"HASH {encoded}")
        log_event(logging.INFO, "put", path=destination, bytes=len(data), stat=stat)
        return {"path": destination, "bytes": len(data), "stat": stat}

    def get(self, source: str, destination: str) -> dict[str, object]:
        encoded = source.encode("ascii").hex().upper()
        with self.agent_lock:
            if self.agent is None:
                raise RuntimeError("TCPAGENT is not connected")
            sock = self.agent
            sock.sendall(f"GET {encoded}\n".encode("ascii"))
            header = self._read_line(sock).decode("ascii", "strict").split()
            if len(header) != 2 or header[0] != "DATA":
                raise RuntimeError("GET failed: " + " ".join(header))
            remaining = int(header[1])
            chunks: list[bytes] = []
            while remaining:
                chunk = sock.recv(min(65536, remaining))
                if not chunk:
                    raise RuntimeError("TCPAGENT closed during GET")
                chunks.append(chunk)
                remaining -= len(chunk)
        target = Path(destination)
        target.parent.mkdir(parents=True, exist_ok=True)
        data = b"".join(chunks)
        target.write_bytes(data)
        log_event(logging.INFO, "get", path=source, bytes=len(data), destination=str(target))
        return {"path": source, "destination": str(target), "bytes": len(data)}

    def monitor(self, command: str) -> str:
        with self.monitor_lock, socket.create_connection(MONITOR_ADDRESS, timeout=3) as sock:
            sock.settimeout(1)
            time.sleep(.1)
            try:
                sock.recv(4096)
            except TimeoutError:
                pass
            sock.sendall(command.encode("ascii") + b"\n")
            time.sleep(.2)
            chunks: list[bytes] = []
            while True:
                try:
                    chunk = sock.recv(4096)
                except TimeoutError:
                    break
                if not chunk:
                    break
                chunks.append(chunk)
        return b"".join(chunks).decode("ascii", "replace").strip()

    def start(self) -> dict[str, object]:
        if self.qemu is not None and self.qemu.poll() is None:
            return {"started": False, "reason": "already running"}
        try:
            self.monitor("info status")
            return {"started": False, "reason": "already running (external)"}
        except OSError:
            pass
        executable = shutil.which("qemu-system-i386.exe")
        disk = QEMU / "freedos.qcow2"
        if not executable:
            raise RuntimeError("qemu-system-i386.exe is not on PATH")
        if not disk.exists():
            raise RuntimeError(f"missing {disk}; run .qemu/setup.ps1 and .qemu/install.ps1")
        self.qemu = subprocess.Popen([executable, "-machine", "pc,accel=whpx,kernel-irqchip=off,usb=on", "-smp", "1", "-m", "64", "-drive", f"file={disk},format=qcow2,if=ide,index=0,media=disk", "-nic", "user,model=ne2k_isa", "-monitor", "tcp:127.0.0.1:4444,server=on,wait=off", "-boot", "order=c", "-display", "default"], cwd=QEMU)
        log_event(logging.INFO, "qemu started", pid=self.qemu.pid)
        return {"started": True, "pid": self.qemu.pid}

    def stop(self) -> dict[str, object]:
        try:
            self.monitor("quit")
            log_event(logging.INFO, "qemu stop requested")
        except OSError:
            pass
        return {"stopped": True}

    def screenshot(self) -> dict[str, object]:
        ppm, png = QEMU / "qemu-screen.ppm", QEMU / "qemu-screen.png"
        self.monitor("screendump " + str(ppm).replace("\\", "/"))
        ffmpeg = shutil.which("ffmpeg.exe")
        if not ppm.exists() or not ffmpeg:
            raise RuntimeError("screenshot failed or ffmpeg.exe is not on PATH")
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(ppm), str(png)], check=True)
        ppm.unlink(missing_ok=True)
        log_event(logging.INFO, "screenshot", path=str(png))
        return {"path": str(png)}

    def dispatch(self, request: dict[str, object]) -> object:
        op = request["op"]
        if op == "status":
            try:
                self.monitor("info status")
                running = True
            except OSError:
                running = False
            return {"agent_connected": self.agent_ready.is_set(), "qemu_running": running, "log": str(LOG_PATH)}
        if op == "start": return self.start()
        if op == "stop": return self.stop()
        if op == "ping": return self.ping()
        if op == "abort": return self.abort()
        if op == "exec":
            return self.exec(str(request["command"]),
                             float(request.get("idle_timeout", DEFAULT_IDLE_TIMEOUT)),
                             float(request.get("hard_timeout", DEFAULT_HARD_TIMEOUT)))
        if op == "put": return self.put(str(request["source"]), str(request["destination"]))
        if op == "get": return self.get(str(request["source"]), str(request["destination"]))
        if op == "screenshot": return self.screenshot()
        if op == "monitor": return {"output": self.monitor(str(request["command"]))}
        raise ValueError(f"unknown operation: {op}")


def serve_pipe(host: Host) -> None:
    listener = Listener(PIPE, family="AF_PIPE")
    log_event(logging.INFO, "control pipe ready", pipe=PIPE)

    def handle(conn) -> None:
        try:
            request = conn.recv()
            try:
                conn.send({"ok": True, "result": host.dispatch(request)})
            except Exception as exc:
                log_event(logging.ERROR, "control failed", error=str(exc))
                conn.send({"ok": False, "error": str(exc)})
        finally:
            conn.close()

    while True:
        # One thread per request: `abort` has to be answerable while a long
        # `exec` is still holding the agent.
        threading.Thread(target=handle, args=(listener.accept(),), daemon=True).start()


def main() -> None:
    if os.name != "nt":
        raise SystemExit("ferro-vm currently supports Windows only")
    configure_logging()
    host = Host()
    try:
        server = host.bind_agent_listener()
    except OSError as exc:
        log_event(logging.ERROR, "agent listener bind failed", address="127.0.0.1:5558", error=str(exc))
        raise SystemExit(f"another ferro-vm daemon already owns 127.0.0.1:5558 ({exc})")
    threading.Thread(target=host.accept_agents, args=(server,), daemon=True).start()
    serve_pipe(host)


if __name__ == "__main__":
    main()
