#!/usr/bin/env python3
"""Build and serve a FreeDOS TCP staging bundle.

The guest pulls STAGE.ZIP with its existing HTGET client from QEMU user-net's
host address (10.0.2.2), then extracts it into C:\\FEC.  This avoids changing
the live vvfat exchange disk and keeps serial for bootstrap/recovery only.
"""
import argparse
import http.server
import os
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_STAGE = ROOT / ".tcp-stage"


def bundle(stage: Path, files: list[str]) -> Path:
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    archive = stage / "STAGE.ZIP"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for item in files:
            source = (ROOT / item).resolve()
            try:
                relative = source.relative_to(ROOT / "fec")
            except ValueError as exc:
                raise SystemExit("only files below fec/ may be staged: " + item) from exc
            if not source.is_file():
                raise SystemExit("not a file: " + item)
            zf.write(source, relative.as_posix())
    print("created %s (%d bytes)" % (archive, archive.stat().st_size))
    return archive


def serve(stage: Path, port: int) -> None:
    if not (stage / "STAGE.ZIP").is_file():
        raise SystemExit("missing STAGE.ZIP; run bundle first")
    handler = lambda *args, **kwargs: http.server.SimpleHTTPRequestHandler(
        *args, directory=str(stage), **kwargs)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    print("serving %s at http://10.0.2.2:%d/STAGE.ZIP" % (stage, port))
    try:
        server.serve_forever()
    finally:
        server.server_close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage-dir", type=Path, default=DEFAULT_STAGE)
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("bundle")
    make.add_argument("files", nargs="+")
    web = commands.add_parser("serve")
    web.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    if args.command == "bundle":
        bundle(args.stage_dir, args.files)
    else:
        serve(args.stage_dir, args.port)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
