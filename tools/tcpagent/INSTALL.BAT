@echo off
if not exist TCPAGENT.EXE goto fail
copy TCPAGENT.EXE C:\FREEDOS\BIN\TCPAGENT.EXE > nul
if errorlevel 1 goto fail
find "TCPAGENT.EXE" C:\FDAUTO.BAT > nul
if not errorlevel 1 goto done
echo SET MTCPCFG=C:\FREEDOS\MTCP.CFG>>C:\FDAUTO.BAT
echo IF EXIST C:\FREEDOS\BIN\TCPAGENT.EXE C:\FREEDOS\BIN\TCPAGENT.EXE>>C:\FDAUTO.BAT
:done
echo TCPAGENT installed.
goto end
:fail
echo TCPAGENT install failed.
verify other 2>nul
:end
