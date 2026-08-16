@echo off
rem Rebuild TCPAGENT.EXE from C:\MTSRC and install it, in one EXEC.
rem BUILD.BAT only runs wmake in the current directory, which is not where a
rem host-driven `ferro-vm exec` starts. Copy this to C:\ and run it by path.
rem The running agent keeps the old image in memory, so reset the VM afterwards:
rem   uv run ferro-vm reset
C:
cd C:\MTSRC\MTCP\APPS\TCPAGENT
set WATCOM=C:\DEVEL\WATCOMC
set PATH=C:\DEVEL\WATCOMC\BINW;C:\DEVEL\WATCOMC\BINP;C:\FREEDOS\BIN
set INCLUDE=C:\DEVEL\WATCOMC\H
set EDPATH=
if exist TCPAGENT.OBJ del TCPAGENT.OBJ
if exist TCPAGENT.EXE del TCPAGENT.EXE
wmake
if not exist TCPAGENT.EXE goto fail
copy /Y TCPAGENT.EXE C:\FREEDOS\BIN\TCPAGENT.EXE
echo BUILD-OK
goto end
:fail
echo BUILD-FAILED
:end
