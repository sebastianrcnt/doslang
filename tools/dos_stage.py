#!/usr/bin/env python3
"""Stage changed source files into the running FreeDOS VM over DOSAGENT.

Usage: python tools/dos_stage.py host/file=C:\\FEC\\DEST.FILE [...]
The DOS agent uses a 115200-baud virtual COM1 link and accepts at most 4096
binary payload bytes per ASCII-hex WRITE request.
"""
import hashlib
import socket
import sys
from pathlib import Path

HOST = "127.0.0.1"
PORT = 5555
DEFAULT_CHUNK_SIZE = 4096


def request(line: str) -> str:
    with socket.create_connection((HOST, PORT), timeout=30) as sock:
        sock.sendall((line + "\n").encode("ascii"))
        response = bytearray()
        while not response.endswith(b"\n"):
            part = sock.recv(65536)
            if not part:
                break
            response.extend(part)
    text = response.decode("ascii", "strict").strip()
    if text.startswith("ERR "):
        raise RuntimeError(bytes.fromhex(text[4:]).decode("utf-8", "replace"))
    if not text.startswith("OK"):
        raise RuntimeError("unexpected DOSAGENT response: " + text)
    return text


def recv_line(sock: socket.socket) -> bytes:
    line = bytearray()
    while not line.endswith(b"\n"):
        part = sock.recv(1)
        if not part:
            raise RuntimeError("TCP agent closed the connection")
        line.extend(part)
    return bytes(line).strip()


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def binary_stage(local: Path, remote: str) -> None:
    data = local.read_bytes()
    path_hex = remote.encode("ascii").hex().upper()
    with socket.create_connection((HOST, PORT), timeout=30) as sock:
        sock.sendall(("PUT %s %d\n" % (path_hex, len(data))).encode("ascii"))
        sock.sendall(data)
        response = recv_line(sock)
        if not response.startswith(b"OK"):
            raise RuntimeError("PUT failed: " + response.decode("ascii", "replace"))
        sock.sendall(("HASH %s\n" % path_hex).encode("ascii"))
        header = recv_line(sock).decode("ascii", "strict")
        fields = header.split()
        if len(fields) != 3 or fields[0] != "STAT":
            raise RuntimeError("HASH failed: " + header)
        remote_length = int(fields[1])
        remote_hash = int(fields[2], 16)
    if remote_length != len(data) or remote_hash != fnv1a(data):
        raise RuntimeError("verification mismatch for " + remote)
    print("staged %s -> %s (%d bytes, sha256 %s, binary TCP)" % (
        local, remote, len(data), hashlib.sha256(data).hexdigest()))


def stage(local: Path, remote: str, chunk_size: int) -> None:
    data = local.read_bytes()
    remote_hex = remote.encode("ascii").hex().upper()
    for offset in range(0, max(1, len(data)), chunk_size):
        part = data[offset : offset + chunk_size]
        mode = "T" if offset == 0 else "A"
        request("WRITE %s %s %s" % (remote_hex, mode, part.hex().upper()))
    remote_data = bytearray()
    offset = 0
    while True:
        response = request("READ %s %u" % (remote_hex, offset))
        _, eof, payload = response.split(" ", 2)
        piece = bytes.fromhex(payload)
        remote_data.extend(piece)
        offset += len(piece)
        if eof == "1":
            break
    if bytes(remote_data) != data:
        raise RuntimeError("verification mismatch for " + remote)
    print("staged %s -> %s (%d bytes, sha256 %s)" % (
        local, remote, len(data), hashlib.sha256(data).hexdigest()))


def main() -> int:
    args = sys.argv[1:]
    chunk_size = DEFAULT_CHUNK_SIZE
    binary = False
    if args and args[0] == "--binary":
        binary = True
        args = args[1:]
    if len(args) >= 2 and args[0] == "--chunk-size":
        chunk_size = int(args[1])
        args = args[2:]
    if not args:
        print("usage: dos_stage.py [--binary] [--chunk-size N] LOCAL=DOS_PATH [... ]", file=sys.stderr)
        return 2
    for spec in args:
        if "=" not in spec:
            raise SystemExit("missing '=' in " + spec)
        local, remote = spec.split("=", 1)
        if binary:
            binary_stage(Path(local), remote)
        else:
            stage(Path(local), remote, chunk_size)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
