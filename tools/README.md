# Development tools

## Host support

Automation currently supports **Windows 10/11 only**. It requires `uv`, QEMU
with WHPX support, and `ffmpeg.exe` on `PATH`. The Python implementation uses
portable APIs where possible, but other hosts are not supported yet.

## QEMU and FreeDOS automation

Start the Python daemon and QEMU:

```powershell
uv run ferro-vm start
uv run ferro-vm status
```

`TCPAGENT.EXE` connects only to `127.0.0.1:5558`. Local commands use the
Windows named pipe `\\.\pipe\ferrolang-vm`; there is no controller or observer
TCP port. Monitor the append-only structured log in another terminal:

```powershell
uv run ferro-vm logs
```

Commands:

```powershell
uv run ferro-vm reset       # clean QEMU quit and restart
uv run ferro-vm wait-ready --timeout 45
uv run ferro-vm ping
uv run ferro-vm exec 'dir C:\FEC'
uv run ferro-vm put fec/src/check.c 'C:\FEC\SRC\CHECK.C'
uv run ferro-vm get 'C:\FEC\TEST.OK' .qemu/TEST.OK
uv run ferro-vm screenshot
uv run ferro-vm ocr
uv run ferro-vm stop
```

`reset` cleanly quits and restarts QEMU, waits for FreeDOS to boot, submits
the default boot-menu Enter, and requires TCPAGENT `PING`/`PONG`. QEMU
`system_reset` is intentionally unsupported because repeated soft resets leave
the FreeDOS NE2000 packet driver stuck during initialization. `logs` starts
`lnav` when installed and otherwise falls back to PowerShell `Get-Content
-Wait`. The daemon logs command metadata, DOS output, exit status, transfers,
and agent lifecycle events as UTF-8 lines. It deliberately never logs raw binary
payloads or protocol hex.

## Standalone OCR

`tools/qemu_ocr.py` remains available for OCRing an existing image:

```powershell
uv run python tools/qemu_ocr.py --image .qemu/qemu-screen.png
```
