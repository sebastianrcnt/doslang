@echo off
rem Open Watcom C89 build. TEST-DOS.BAT runs this from C:\FEC.
C:
cd \FEC
if exist BUILD.OK del BUILD.OK
if exist BUILD.FAIL del BUILD.FAIL
if exist fec.exe del fec.exe
if exist __wcl__.lnk del __wcl__.lnk
if exist arena.obj del arena.obj
if exist diag.obj del diag.obj
if exist lexer.obj del lexer.obj
if exist ast.obj del ast.obj
if exist parser.obj del parser.obj
if exist driver.obj del driver.obj

if "%WATCOM%"=="" set WATCOM=C:\DEVEL\WATCOMC
if not exist %WATCOM%\BINW\WCL.EXE goto build_fail
set PATH=%WATCOM%\BINW;%WATCOM%\BINP;%PATH%
wcl -q -za -wx -bt=dos -k32768 -fe=fec.exe src\arena.c src\diag.c src\lexer.c src\ast.c src\parser.c src\driver.c
if errorlevel 1 goto build_fail
if not exist fec.exe goto build_fail
echo OK>BUILD.OK
cd C:\FEC
goto build_done

:build_fail
echo FAIL>BUILD.FAIL

:build_done
