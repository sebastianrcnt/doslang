@echo off
rem Pull a host-built TCP staging bundle into the authoritative C:\FEC tree.
rem QEMU user networking exposes the host as 10.0.2.2.  HTGET and UNZIP are
rem supplied by the installed FreeDOS network tools.
if "%1"=="" goto usage
if exist C:\FEC\STAGE.ZIP del C:\FEC\STAGE.ZIP
htget -o C:\FEC\STAGE.ZIP %1
if errorlevel 1 goto fail
unzip -o C:\FEC\STAGE.ZIP -d C:\FEC
if errorlevel 1 goto fail
if exist C:\FEC\TCPSTG.OK del C:\FEC\TCPSTG.OK
echo OK>C:\FEC\TCPSTG.OK
goto done
:usage
echo usage: TCP-STAGE.BAT http://10.0.2.2:8000/STAGE.ZIP
goto done
:fail
echo FAIL>C:\FEC\TCPSTG.FAIL
:done
