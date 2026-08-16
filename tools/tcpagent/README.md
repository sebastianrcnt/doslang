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

`PING`, `READ`, `WRITE`, `LIST`, and `EXEC` use text commands. Fast transfer
commands are:

- `PUT <hex DOS path> <byte length>\n<raw bytes>` -> `OK\r\n`
- `GET <hex DOS path>\n` -> `DATA <length>\r\n<raw bytes>`
- `HASH <hex DOS path>\n` -> `STAT <length> <FNV1A32>\r\n`

The host invokes them through:

```powershell
uv run ferro-vm put host-file 'C:\DOS\FILE'
uv run ferro-vm get 'C:\DOS\FILE' host-file
```

No DOS-side change is required for the Python host automation.
