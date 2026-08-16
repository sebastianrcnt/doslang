"""Long-lived Windows host for the FreeDOS TCP agent.

The only automation TCP listener is 127.0.0.1:5558, used exclusively by
TCPAGENT.EXE. Local commands use a Windows named pipe.
"""
from __future__ import annotations

import json
import logging
import os
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

    def accept_agents(self) -> None:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(AGENT_ADDRESS)
        server.listen(1)
        log_event(logging.INFO, "agent listener ready", address="127.0.0.1:5558")
        while True:
            sock, peer = server.accept()
            sock.settimeout(30)
            with self.agent_lock:
                if self.agent is not None:
                    sock.close()
                    log_event(logging.WARNING, "agent rejected", peer=str(peer), reason="already connected")
                    continue
                self.agent = sock
                self.agent_ready.set()
            try:
                banner = self._read_line(sock).decode("ascii", "replace")
                log_event(logging.INFO, "agent connected", peer=f"{peer[0]}:{peer[1]}", banner=banner)
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
            finally:
                with self.agent_lock:
                    if self.agent is sock:
                        self.agent = None
                        self.agent_ready.clear()
                sock.close()
                log_event(logging.INFO, "agent disconnected", peer=f"{peer[0]}:{peer[1]}")

    @staticmethod
    def _read_line(sock: socket.socket) -> bytes:
        out = bytearray()
        while not out.endswith(b"\n"):
            part = sock.recv(1)
            if not part:
                raise ConnectionError("TCP agent closed connection")
            out.extend(part)
        return bytes(out).rstrip(b"\r\n")

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

    def exec(self, command: str) -> dict[str, object]:
        log_event(logging.INFO, "exec start", command=command)
        response = self.request("EXEC " + command.encode("ascii", "replace").hex().upper())
        fields = response.split(" ", 2)
        code = int(fields[1]) if len(fields) > 1 else -1
        output = bytes.fromhex(fields[2]).decode("cp437", "replace") if len(fields) > 2 else ""
        for line in output.splitlines():
            log_event(logging.INFO, "dos output", line=line)
        log_event(logging.INFO, "exec finish", exit=code)
        return {"exit": code, "output": output}

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

    @staticmethod
    def monitor(command: str) -> str:
        with socket.create_connection(MONITOR_ADDRESS, timeout=3) as sock:
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
        if op == "soft-reset":
            self.monitor("system_reset")
            log_event(logging.INFO, "qemu soft reset requested")
            return {"reset": "requested"}
        if op == "ping": return self.ping()
        if op == "exec": return self.exec(str(request["command"]))
        if op == "put": return self.put(str(request["source"]), str(request["destination"]))
        if op == "get": return self.get(str(request["source"]), str(request["destination"]))
        if op == "screenshot": return self.screenshot()
        if op == "monitor": return {"output": self.monitor(str(request["command"]))}
        raise ValueError(f"unknown operation: {op}")


def serve_pipe(host: Host) -> None:
    listener = Listener(PIPE, family="AF_PIPE")
    log_event(logging.INFO, "control pipe ready", pipe=PIPE)
    while True:
        conn = listener.accept()
        try:
            request = conn.recv()
            try:
                conn.send({"ok": True, "result": host.dispatch(request)})
            except Exception as exc:
                log_event(logging.ERROR, "control failed", error=str(exc))
                conn.send({"ok": False, "error": str(exc)})
        finally:
            conn.close()


def main() -> None:
    if os.name != "nt":
        raise SystemExit("ferro-vm currently supports Windows only")
    configure_logging()
    host = Host()
    threading.Thread(target=host.accept_agents, daemon=True).start()
    serve_pipe(host)


if __name__ == "__main__":
    main()
