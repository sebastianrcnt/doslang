#!/usr/bin/env python3
"""Capture the QEMU VGA console and print its text with RapidOCR."""

from __future__ import annotations

import argparse
import json
import logging
import subprocess
import sys
from pathlib import Path

from rapidocr import RapidOCR

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_IMAGE = ROOT / ".qemu" / "qemu-screen.png"
SCREENSHOT = ROOT / ".qemu" / "screenshot.ps1"


def capture() -> Path:
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(SCREENSHOT),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )
    if not DEFAULT_IMAGE.is_file():
        raise RuntimeError(f"QEMU screenshot was not created: {DEFAULT_IMAGE}")
    return DEFAULT_IMAGE


def recognize(image: Path, min_score: float) -> list[dict[str, object]]:
    logging.disable(logging.INFO)
    result = RapidOCR()(image)
    rows: list[dict[str, object]] = []
    if result.txts is None or result.boxes is None or result.scores is None:
        return rows
    for box, text, score in zip(result.boxes, result.txts, result.scores):
        if float(score) < min_score:
            continue
        rows.append(
            {
                "x": int(min(point[0] for point in box)),
                "y": int(min(point[1] for point in box)),
                "text": text,
                "score": round(float(score), 5),
            }
        )
    rows.sort(key=lambda row: (int(row["y"]), int(row["x"])))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, help="OCR an existing image instead of capturing QEMU")
    parser.add_argument("--min-score", type=float, default=0.5)
    parser.add_argument("--json", action="store_true", help="emit locations and scores as JSON")
    parser.add_argument("-o", "--output", type=Path, help="also save output as UTF-8 text")
    args = parser.parse_args()

    image = args.image.resolve() if args.image else capture()
    rows = recognize(image, args.min_score)
    if args.json:
        rendered = json.dumps({"image": str(image), "lines": rows}, ensure_ascii=False, indent=2)
    else:
        rendered = "\n".join(str(row["text"]) for row in rows)
    if args.output:
        args.output.write_text(rendered + ("\n" if rendered else ""), encoding="utf-8")
    if rendered:
        print(rendered)
    return 0 if rows else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"qemu-ocr: {exc}", file=sys.stderr)
        raise SystemExit(2)
