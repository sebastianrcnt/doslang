# TODO

## Done

- [x] Add a non-reboot abort path for a hung DOS command: inject `Ctrl+C` through
      QEMU's monitor and wait for the agent to recover.
- [x] Apply a configurable timeout to `exec` and invoke the non-reboot abort path
      on timeout.
- [x] Expose `abort` for an immediate user-requested command interruption.

The three above were first built for the COM1 serial agent, lost in the rewrite
to the resident TCP agent, and rebuilt on the QEMU monitor in `ferro-vm exec`.
The TCP version supervises with guest disk liveness (`info blockstats`) rather
than a fixed stopwatch, so a slow compile is no longer mistaken for a hang.

## Open

- [ ] Consider `BREAK=ON` in `C:\FDCONFIG.SYS`. Ctrl+C only takes effect at a DOS
      break check, and with the FreeDOS default of `BREAK=OFF` a compute-bound
      child whose output is redirected to a file may never reach one, so `abort`
      cannot always stop it. `BREAK=ON` checks on every DOS call and makes the
      abort reliable, at a small cost to every DOS call. Needs a VM reboot; back
      up `FDCONFIG.SYS` first.
- [ ] `TCPAGENT.EXE` connects from a fixed source port (`LOCAL_PORT 2058`). After
      the host end closes, a reconnect reuses the same 4-tuple and can flap until
      the old state ages out. Observed as a ~10s connect/disconnect cycle after
      the daemon is killed mid-connection.
