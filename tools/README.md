# Development tools

## Host support

The automated DOS development environment currently supports Windows 10/11.
The host only needs `uv`. The setup command downloads the pinned DOSBox-X and
Open Watcom DOS archives, verifies their SHA-256 hashes, and installs them in the
ignored `.dosboxx/` cache.

```powershell
uv run ferro-dos setup --accept-watcom-license
```

Review the Open Watcom license referenced by
`tools/toolchains/dosboxx.lock.json` before accepting it. Neither downloaded
archives nor installed tools are committed.

## General DOS environment

`ferro-dos` provides the development entry points:

```powershell
uv run ferro-dos build
uv run ferro-dos exec "FEC.EXE --check TESTS\M6\OKLAST.FE"
uv run ferro-dos batch fec\test-dos.bat
uv run ferro-dos shell
uv run ferro-dos --help
```

Every invocation creates an isolated host directory under `.dosboxx/runs/` and
mounts it as writable `C:`. The repository is mounted read-only as `R:` and the
pinned Open Watcom installation as read-only `W:`. Current compiler sources,
the standard library, and fixtures are copied to `C:\FEC`; all compilation and
execution happen there inside DOSBox-X. Successful runs are removed by default.
Use `--keep` to retain a workspace and `--show-dos` to display the DOS window.

This directory-backed layout deliberately has no QEMU, disk-image, TCP-agent,
or OCR dependency. A future disk-image backend can be added without changing
the command interface.

## Pytest regression suite

`ferro-test` uses the same isolated DOSBox-X/Open Watcom environment, builds
`FEC.EXE` once, and executes all selected cases sequentially in that one DOS
instance. Pytest still reports each registered case separately.

```powershell
uv run ferro-test run --through m6 -v
uv run ferro-test run --only m6 --dos-log
uv run ferro-test --help
```

`--keep-failed` preserves a failed workspace, `--dos-log` prints captured DOS
output, `--trace-dos` disables per-command redirection, and `--show-dos` displays
the GUI. Working rules and DOS/Open Watcom build traps are in `AGENTS.md`.
