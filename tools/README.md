# Development tools

## Host support

Automation currently supports **Windows 10/11 only**. It requires `uv`, QEMU
with WHPX support, and `ffmpeg.exe` on `PATH`. The Python implementation uses
portable APIs where possible, but other hosts are not supported yet.

## Getting started

```powershell
uv run ferro-vm start
uv run ferro-vm status
```

The command list lives in the CLI itself, not in this file:

```powershell
uv run ferro-vm --help
uv run ferro-vm <command> --help
```

Working rules, verification gates, and DOS build traps are in `AGENTS.md`.

## How it fits together

`TCPAGENT.EXE` runs inside FreeDOS and dials out to `127.0.0.1:5558`; its wire
protocol is documented in `tcpagent/README.md`. The `ferro-vm` daemon owns that
connection and the QEMU monitor. Local commands reach the daemon over the
Windows named pipe `\\.\pipe\ferrolang-vm` — there is no controller or observer
TCP port.

The daemon writes an append-only structured log (`uv run ferro-vm logs`, which
uses `lnav` when installed and otherwise falls back to PowerShell `Get-Content
-Wait`). It records command metadata, DOS output, exit status, transfers, and
agent lifecycle events as UTF-8 lines, and deliberately never logs raw binary
payloads or protocol hex.

`reset` quits QEMU cleanly, restarts it, waits for FreeDOS to boot, submits the
default boot-menu Enter, and requires a TCPAGENT `PING`/`PONG` before returning.
QEMU `system_reset` is intentionally unsupported: repeated soft resets leave the
FreeDOS NE2000 packet driver stuck during initialization.

## Standalone OCR

`tools/qemu_ocr.py` remains available for OCRing an existing image:

```powershell
uv run python tools/qemu_ocr.py --image .qemu/qemu-screen.png
```
