# FreeDOS resident TCP agent

`TCPAGENT.EXE` is a foreground resident automation process. It uses the mTCP
packet-driver stack and maintains an outbound connection to QEMU's host at
`10.0.2.2:5558`. The host relay keeps the existing Pi tool endpoint on 5555.

## Build in FreeDOS

1. Obtain the GPLv3 mTCP source tree (tested with the jhpyle/mTCP 2022 fork).
2. Copy this directory to `MTCP\APPS\TCPAGENT` inside that tree.
3. Set `WATCOM` for Open Watcom and run `BUILD.BAT`.
4. Run `INSTALL.BAT`; it installs the executable and adds startup lines after
   the existing packet-driver setup in `C:\FDAUTO.BAT`.

The build uses mTCP's compact memory model and Open Watcom C++16. The agent is
therefore distributed under GPLv3 when linked with mTCP.

## Protocol

Legacy text commands remain for Pi tool compatibility: `PING`, `READ`,
`WRITE`, `LIST`, and `EXEC`. Fast staging uses binary framing:

- `PUT <hex DOS path> <byte length>\n<raw bytes>` -> `OK\r\n`
- `GET <hex DOS path>\n` -> `DATA <length>\r\n<raw bytes>`
- `HASH <hex DOS path>\n` -> `STAT <length> <FNV1A32>\r\n`

Use `python tools/dos_stage.py --binary LOCAL=C:\\DOS\\PATH` for verified
staging. Verification reads only the length and FNV-1a hash, avoiding a slow
full-file return transfer.

Use `.qemu/reset.ps1` to restart QEMU. It waits for an established TCP agent
and a successful PONG rather than sleeping for a fixed boot duration.
