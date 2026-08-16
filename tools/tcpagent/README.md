# FreeDOS resident TCP agent

`TCPAGENT.EXE` is a foreground resident automation process. It uses the mTCP
packet-driver stack and maintains an outbound connection to the QEMU host at
`10.0.2.2:5558`.

The Windows-only Python `ferro-vm` daemon owns that listener. It logs metadata
and decoded command output to `.qemu/ferro-vm.log`; it does not expose an
observer/controller TCP port or emit binary payloads to the log. Local host
control uses a Windows named pipe.

## Build in FreeDOS

1. Obtain the GPLv3 mTCP source tree (tested with the jhpyle/mTCP 2022 fork).
2. Copy this directory to `MTCP\APPS\TCPAGENT` inside that tree.
3. Set `WATCOM` for Open Watcom and run `BUILD.BAT`.
4. Run `INSTALL.BAT`; it installs the executable and adds startup lines after
   the existing packet-driver setup in `C:\FDAUTO.BAT`.

The build uses mTCP's compact memory model and Open Watcom C++16. The agent is
therefore distributed under GPLv3 when linked with mTCP.

## Protocol

`PING`, `READ`, `WRITE`, and `LIST` use text commands. `EXEC` captures both
stdout and stderr at the DOS handle level and returns an untruncated raw body:

- `EXEC <hex command>\n` -> `RESULT <exit> <length> <flags>\r\n<raw bytes>`

Fast transfer commands are:

- `PUT <hex DOS path> <byte length>\n<raw bytes>` -> `OK\r\n`
- `GET <hex DOS path>\n` -> `DATA <length>\r\n<raw bytes>`
- `HASH <hex DOS path>\n` -> `STAT <length> <FNV1A32>\r\n`

The host invokes them through:

```powershell
uv run ferro-vm put host-file 'C:\DOS\FILE'
uv run ferro-vm get 'C:\DOS\FILE' host-file
```

## Agent-side logging

The foreground agent prints one timestamped line per event on the VGA console
and keeps the same text in `C:\TCPAGENT.LOG`, rotating files larger than 256 KiB
to `C:\TCPAGENT.OLD`. Payloads and command output are never written to that
metadata log.

Every command is logged with a request line and a result line carrying byte
counts and elapsed time — `EXEC`, `PUT`, `GET`, `HASH`, `LIST`, `READ`, and
`WRITE`. `PING` is deliberately excluded because `wait-ready` polls it twice a
second. Connection events (`connecting`, `connected`, `connect failed; retry N`,
`link lost`) are logged too; those are invisible to the host by definition,
since they happen when the socket is down.

Lines are colored by writing VGA attribute bytes after `cprintf` lays out the
line: gray timestamps, cyan requests, yellow `EXEC` command text, green success,
red failure. Open Watcom's DOS `conio.h` has no `textattr()`, and ANSI escapes
are not interpreted on this FreeDOS console, so neither of the usual routes
works. Elapsed times come from the BIOS tick counter at 18.2065 Hz (~55 ms
resolution).

Note that mTCP is not driven while `system()` runs a child, so a DOS command
lasting tens of seconds can drop the TCP connection. The agent logs `link lost`
and reconnects on its own, but the host loses that command's result.

## Rebuilding inside the VM

`REBUILD.BAT` compiles and installs the agent in a single `exec`. `BUILD.BAT`
only runs `wmake` in the current directory, which is not where a host-driven
`ferro-vm exec` starts.

```powershell
uv run ferro-vm put tools/tcpagent/tcpagent.cpp 'C:\MTSRC\MTCP\APPS\TCPAGENT\TCPAGENT.CPP'
uv run ferro-vm put tools/tcpagent/REBUILD.BAT 'C:\REBUILD.BAT'
uv run ferro-vm exec 'C:\REBUILD.BAT'
uv run ferro-vm reset
```

The reset is required: the running agent holds the old image in memory, and
`C:\FDAUTO.BAT` starts it at boot.
