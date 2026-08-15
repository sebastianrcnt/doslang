# DOS VM tools

This project-local Pi extension exposes `dos_read`, `dos_write`, `dos_edit`,
`dos_list`, and `dos_exec`. It communicates with `DOSAGENT.EXE` over COM1
through the serial relay controller port at `127.0.0.1:5555`.

The agent is not started automatically. From the FreeDOS VGA console, run
`DOSAGENT` (installed in `C:\\FREEDOS\\BIN`, which is on `PATH`) when Pi tool
access is needed. While it runs, press Ctrl+C in the VGA console to stop it and
return to the DOS prompt; Pi tools then cannot connect until it is started
again. QEMU monitor control remains available at `127.0.0.1:4444`.

Connect PuTTY in Raw mode to `127.0.0.1:5557` for a read-only decoded mirror of
Pi/DOS serial traffic. Port 5556 is reserved for the internal QEMU-to-relay
connection.

`D:` is QEMU's FAT-directory view of `.qemu\\share`. Treat it as an exchange
volume, not a live-sync mount: host-side changes can be stale or partially
visible to a running VM. Restart QEMU before compiling host-edited files, or
use `dos_write` to place the source on `C:` before compiling.
