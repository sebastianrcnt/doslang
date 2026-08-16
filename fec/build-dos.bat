@echo off
rem Open Watcom C89 build. TEST-DOS.BAT runs this from C:\FEC.
C:
cd \FEC
if exist BUILD.OK del BUILD.OK
if exist BUILD.FAIL del BUILD.FAIL
if exist fec.exe del fec.exe
if exist __wcl__.lnk del __wcl__.lnk
if exist *.obj del *.obj

if "%WATCOM%"=="" set WATCOM=C:\DEVEL\WATCOMC
if not exist %WATCOM%\BINW\WCL.EXE goto build_fail
set PATH=%WATCOM%\BINW;%WATCOM%\BINP;%PATH%
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=arena.obj src\arena.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=diag.obj src\diag.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=lexer.obj src\lexer.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=ast.obj src\ast.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=parser.obj src\parser.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=types.obj src\types.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=m7.obj src\m7.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=own.obj src\own.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=check.obj src\check_m7.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=lower.obj src\lower.c
if errorlevel 1 goto build_fail
rem Use an unambiguous short object name for the M7 emitter source.
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=emitc.obj src\emit_c_m7.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=driver.obj src\driver.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -fe=fec.exe *.obj
if errorlevel 1 goto build_fail
if not exist fec.exe goto build_fail
echo OK>BUILD.OK
cd C:\FEC
goto build_done

:build_fail
echo FAIL>BUILD.FAIL

:build_done
