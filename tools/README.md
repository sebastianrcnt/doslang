# Development tools

## Host support

Automation currently supports **Windows 10/11 only**. The fast development loop
requires only `uv`; it downloads pinned DOSBox-X and Open Watcom DOS releases.
The final milestone gate additionally requires QEMU with WHPX support and
`ffmpeg.exe` on `PATH`. Other hosts are not supported yet.

## Getting started

```powershell
uv run ferro-test setup --accept-watcom-license
uv run ferro-test run --through m6 -v
```

`setup` reads `tools/toolchains/dosboxx.lock.json`, downloads the exact official
archives, verifies their SHA-256 hashes, and extracts them under ignored
`.dosboxx/`. Review the Open Watcom license referenced by the lock file before
accepting it. Archives and installed tools are deliberately not committed.

Each `run` creates a disposable DOS drive, copies the current compiler, standard
library, and fixtures, then builds `FEC.EXE` once inside DOS with Open Watcom.
All selected milestone commands execute sequentially in that same DOSBox-X
instance, while pytest reports every emit, Watcom build, runtime, and rejection
check separately. Thus stale QEMU binaries cannot make the test pass.

`--through m6` runs cumulatively from M1; `--only m6` selects one milestone.
Use `--keep-failed` to preserve a failed drive under `.dosboxx/runs/`,
`--dos-log` to print the captured DOS console, `--trace-dos` to disable command
output redirection, and `--show-dos` to keep the GUI open until a key is pressed.

This is the quick development smoke test. Run the QEMU/FreeDOS workflow below
for the authoritative milestone completion gate.

```powershell
uv run ferro-vm start
uv run ferro-vm status
```

The command list lives in the CLI itself, not in this file:

```powershell
uv run ferro-vm --help
uv run ferro-vm <command> --help
uv run ferro-test --help
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
