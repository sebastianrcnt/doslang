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

if exist TESTS\M3\STRUCT.C del TESTS\M3\STRUCT.C
if exist TESTS\M3\STRUCT.EXE del TESTS\M3\STRUCT.EXE
if exist TESTS\M3\ENUM.C del TESTS\M3\ENUM.C
if exist TESTS\M3\ENUM.EXE del TESTS\M3\ENUM.EXE
if exist TESTS\M3\ARRAY.C del TESTS\M3\ARRAY.C
if exist TESTS\M3\ARRAY.EXE del TESTS\M3\ARRAY.EXE
if exist TESTS\M3\STR.C del TESTS\M3\STR.C
if exist TESTS\M3\STR.EXE del TESTS\M3\STR.EXE
if exist TESTS\M3\FOR.C del TESTS\M3\FOR.C
if exist TESTS\M3\FOR.EXE del TESTS\M3\FOR.EXE
if exist TESTS\M3\NESTED.C del TESTS\M3\NESTED.C
if exist TESTS\M3\NESTED.EXE del TESTS\M3\NESTED.EXE
if exist TESTS\M3\CHAR.C del TESTS\M3\CHAR.C
if exist TESTS\M3\CHAR.EXE del TESTS\M3\CHAR.EXE
if exist TESTS\M3\ARRAYCTX.C del TESTS\M3\ARRAYCTX.C
if exist TESTS\M3\ARRAYCTX.EXE del TESTS\M3\ARRAYCTX.EXE
if exist TESTS\M3\BOUNDS.C del TESTS\M3\BOUNDS.C
if exist TESTS\M3\BOUNDS.EXE del TESTS\M3\BOUNDS.EXE
if exist TESTS\M3\BOUNDS-N.C del TESTS\M3\BOUNDS-N.C
if exist TESTS\M3\BOUNDS-N.EXE del TESTS\M3\BOUNDS-N.EXE

fec.exe --target=bits32 --emit-c TESTS\M3\STRUCT.FE -o TESTS\M3\STRUCT.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\STRUCT.EXE TESTS\M3\STRUCT.C
if errorlevel 1 goto test_fail
TESTS\M3\STRUCT.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\ENUM.FE -o TESTS\M3\ENUM.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\ENUM.EXE TESTS\M3\ENUM.C
if errorlevel 1 goto test_fail
TESTS\M3\ENUM.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\ARRAY.FE -o TESTS\M3\ARRAY.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\ARRAY.EXE TESTS\M3\ARRAY.C
if errorlevel 1 goto test_fail
TESTS\M3\ARRAY.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\STR.FE -o TESTS\M3\STR.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\STR.EXE TESTS\M3\STR.C
if errorlevel 1 goto test_fail
TESTS\M3\STR.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\FOR.FE -o TESTS\M3\FOR.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\FOR.EXE TESTS\M3\FOR.C
if errorlevel 1 goto test_fail
TESTS\M3\FOR.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\NESTED.FE -o TESTS\M3\NESTED.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\NESTED.EXE TESTS\M3\NESTED.C
if errorlevel 1 goto test_fail
TESTS\M3\NESTED.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\CHAR.FE -o TESTS\M3\CHAR.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\CHAR.EXE TESTS\M3\CHAR.C
if errorlevel 1 goto test_fail
TESTS\M3\CHAR.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\ARRAYCTX.FE -o TESTS\M3\ARRAYCTX.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\ARRAYCTX.EXE TESTS\M3\ARRAYCTX.C
if errorlevel 1 goto test_fail
TESTS\M3\ARRAYCTX.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BOUNDS.FE -o TESTS\M3\BOUNDS.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\BOUNDS.EXE TESTS\M3\BOUNDS.C
if errorlevel 1 goto test_fail
TESTS\M3\BOUNDS.EXE
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --no-checks --emit-c TESTS\M3\BOUNDS.FE -o TESTS\M3\BOUNDS-N.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\BOUNDS-N.EXE TESTS\M3\BOUNDS-N.C
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADFLD.FE -o TESTS\M3\BADFLD.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADMAT.FE -o TESTS\M3\BADMAT.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADARR.FE -o TESTS\M3\BADARR.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADCYCLE.FE -o TESTS\M3\BADCYCLE.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADSTR.FE -o TESTS\M3\BADSTR.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADCHAR.FE -o TESTS\M3\BADCHAR.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADFIELD.FE -o TESTS\M3\BADFIELD.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BADINDEX.FE -o TESTS\M3\BADINDEX.C > nul
if not errorlevel 1 goto test_fail

echo OK>TEST.OK
cd C:\FEC
goto test_done

:test_fail
echo FAIL>TEST.FAIL
verify other 2>nul

:test_done
