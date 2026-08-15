@echo off
rem FreeDOS smoke tests. All work happens on the writable C: drive.
C:
cd \FEC
if exist TEST.OK del TEST.OK
if exist TEST.FAIL del TEST.FAIL
call C:\FEC\BUILD.BAT
if not exist BUILD.OK goto test_fail
if "%WATCOM%"=="" set WATCOM=C:\DEVEL\WATCOMC
if not exist %WATCOM%\BINW\WCL.EXE goto test_fail
if not exist %WATCOM%\BINW\WCL386.EXE goto test_fail
set PATH=%WATCOM%\BINW;%WATCOM%\BINP;%PATH%

fec.exe --dump-ast TESTS\PASS\BASIC.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\PASS\LITERALS.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\PASS\KEYWOR.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\PASS\V012-F.FE > nul
if errorlevel 1 goto test_fail

fec.exe --dump-ast STD\CORE.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\FMT.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\IO.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\LIST.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\MAP.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\MEM.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\STR.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast STD\SYS.FE > nul
if errorlevel 1 goto test_fail

fec.exe --dump-ast TESTS\FAIL\MISSIN.FE > nul
if not errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\FAIL\UNCLOS.FE > nul
if not errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\FAIL\LOGICA.FE > nul
if not errorlevel 1 goto test_fail

if exist TESTS\M2\HELLO.C del TESTS\M2\HELLO.C
if exist TESTS\M2\HELLO.EXE del TESTS\M2\HELLO.EXE
if exist TESTS\M2\SCOPES.C del TESTS\M2\SCOPES.C
if exist TESTS\M2\SCOPES.EXE del TESTS\M2\SCOPES.EXE
if exist TESTS\M2\CAST16.C del TESTS\M2\CAST16.C
if exist TESTS\M2\CAST16.EXE del TESTS\M2\CAST16.EXE

rem M2 bits32 path: Open Watcom 32-bit compiler and DOS extender executable.
fec.exe --target=bits32 --emit-c TESTS\M2\HELLO.FE -o TESTS\M2\HELLO.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M2\HELLO.EXE TESTS\M2\HELLO.C
if errorlevel 1 goto test_fail
TESTS\M2\HELLO.EXE
if errorlevel 1 goto test_fail

fec.exe --target=bits32 --emit-c TESTS\M2\SCOPES.FE -o TESTS\M2\SCOPES.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M2\SCOPES.EXE TESTS\M2\SCOPES.C
if errorlevel 1 goto test_fail
TESTS\M2\SCOPES.EXE
if errorlevel 1 goto test_fail

rem M2 bits16 regression path remains on compiler A (wcl).
fec.exe --target=bits16 --emit-c TESTS\M2\CAST-W.FE -o TESTS\M2\CAST16.C > nul
if errorlevel 1 goto test_fail
wcl -q -za -bt=dos -fe=TESTS\M2\CAST16.EXE TESTS\M2\CAST16.C
if errorlevel 1 goto test_fail
TESTS\M2\CAST16.EXE
if errorlevel 1 goto test_fail

fec.exe --target=bits32 --emit-c TESTS\M2\BAD-CO.FE -o TESTS\M2\BAD-CO.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-CA.FE -o TESTS\M2\BAD-CA.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-AS.FE -o TESTS\M2\BAD-AS.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-UN.FE -o TESTS\M2\BAD-UN.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-AR.FE -o TESTS\M2\BAD-AR.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-TY.FE -o TESTS\M2\BAD-TY.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-RE.FE -o TESTS\M2\BAD-RE.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-UI.FE -o TESTS\M2\BAD-UI.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-VO.FE -o TESTS\M2\BAD-VO.C > nul
if not errorlevel 1 goto test_fail

echo OK>TEST.OK
cd C:\FEC
goto test_done

:test_fail
echo FAIL>TEST.FAIL
verify other 2>nul

:test_done
