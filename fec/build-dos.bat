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
if exist types.obj del types.obj
if exist check.obj del check.obj
if exist emitc.obj del emitc.obj
if exist emit_c.obj del emit_c.obj
if exist driver.obj del driver.obj

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
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=check.obj src\check.c
if errorlevel 1 goto build_fail
rem Use an unambiguous short object name for the emit_c source.
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=emitc.obj src\emit_c.c
if errorlevel 1 goto build_fail
wcl -q -za -wx -bt=dos -ml -k32768 -c -fo=driver.obj src\driver.c
if errorlevel 1 goto build_fail
rem DOS command lines are limited to roughly 126 characters. All stale objects
rem were removed above, so the wildcard contains exactly this build's objects.
wcl -q -za -wx -bt=dos -ml -k32768 -fe=fec.exe *.obj
if errorlevel 1 goto build_fail
if not exist fec.exe goto build_fail
echo OK>BUILD.OK
cd C:\FEC
goto build_done

:build_fail
echo FAIL>BUILD.FAIL

:build_done
