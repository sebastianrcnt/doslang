# TODO

- [x] Add a non-reboot abort path for a hung DOS command: inject `Ctrl+C` through QEMU's monitor and wait for the serial agent to recover.
- [x] Apply a configurable timeout to `dos_exec` and invoke the non-reboot abort path on timeout.
- [x] Expose `dos_abort` for an immediate user-requested command interruption.
