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
CHUNK_SIZE = 4096


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


def stage(local: Path, remote: str) -> None:
    data = local.read_bytes()
    remote_hex = remote.encode("ascii").hex().upper()
    for offset in range(0, max(1, len(data)), CHUNK_SIZE):
        part = data[offset : offset + CHUNK_SIZE]
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
    if len(sys.argv) < 2:
        print("usage: dos_stage.py LOCAL=DOS_PATH [... ]", file=sys.stderr)
        return 2
    for spec in sys.argv[1:]:
        if "=" not in spec:
            raise SystemExit("missing '=' in " + spec)
        local, remote = spec.split("=", 1)
        stage(Path(local), remote)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
