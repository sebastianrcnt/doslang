# Development tools

## QEMU console OCR

Capture the current QEMU VGA screen and print detected console text:

```powershell
uv run python tools/qemu_ocr.py
```

Useful options:

```powershell
# Machine-readable boxes, confidence scores, and text
uv run python tools/qemu_ocr.py --json

# OCR an existing screenshot without recapturing
uv run python tools/qemu_ocr.py --image .qemu/qemu-screen.png

# Save extracted text
uv run python tools/qemu_ocr.py -o .qemu/qemu-screen.txt
```

The tool invokes `.qemu/screenshot.ps1`, runs RapidOCR with ONNX Runtime, sorts
recognized lines by screen position, and emits UTF-8 text. Screenshots and OCR
output under `.qemu/` remain ignored build artifacts.
