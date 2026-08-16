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
fec.exe --dump-ast TESTS\PASS\KEYBUILT.FE > nul
if errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\PASS\V012FORM.FE > nul
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

fec.exe --dump-ast TESTS\FAIL\MISSSEMI.FE > nul
if not errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\FAIL\UNCLCOMM.FE > nul
if not errorlevel 1 goto test_fail
fec.exe --dump-ast TESTS\FAIL\LOGICAL.FE > nul
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
fec.exe --target=bits16 --emit-c TESTS\M2\CASTWHIL.FE -o TESTS\M2\CAST16.C > nul
if errorlevel 1 goto test_fail
wcl -q -za -bt=dos -fe=TESTS\M2\CAST16.EXE TESTS\M2\CAST16.C
if errorlevel 1 goto test_fail
TESTS\M2\CAST16.EXE
if errorlevel 1 goto test_fail

fec.exe --target=bits32 --emit-c TESTS\M2\BAD-COND.FE -o TESTS\M2\BAD-CO.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-CAST.FE -o TESTS\M2\BAD-CA.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-ASGN.FE -o TESTS\M2\BAD-AS.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-UNK.FE -o TESTS\M2\BAD-UN.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-ARI.FE -o TESTS\M2\BAD-AR.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-TYPE.FE -o TESTS\M2\BAD-TY.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-RET.FE -o TESTS\M2\BAD-RE.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-UNIT.FE -o TESTS\M2\BAD-UI.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M2\BAD-VOID.FE -o TESTS\M2\BAD-VO.C > nul
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
fec.exe --target=bits32 --emit-c TESTS\M3\MUTABLE.FE -o TESTS\M3\MUTABLE.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\MUTABLE.EXE TESTS\M3\MUTABLE.C
if errorlevel 1 goto test_fail
TESTS\M3\MUTABLE.EXE
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BAD-MLET.FE -o TESTS\M3\BAD-MLET.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M3\BAD-SHWR.FE -o TESTS\M3\BAD-SHWR.C > nul
if not errorlevel 1 goto test_fail
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
fec.exe --target=bits32 --emit-c TESTS\M3\SLCBOUND.FE -o TESTS\M3\SLCBOUND.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -bt=dos -fe=TESTS\M3\SLCBOUND.EXE TESTS\M3\SLCBOUND.C
if errorlevel 1 goto test_fail
TESTS\M3\SLCBOUND.EXE
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

if exist TESTS\M4\FORMAT.C del TESTS\M4\FORMAT.C
if exist TESTS\M4\FORMAT.EXE del TESTS\M4\FORMAT.EXE
fec.exe --target=bits32 --emit-c TESTS\M4\FORMAT.FE -o TESTS\M4\FORMAT.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\M4\FORMAT.EXE TESTS\M4\FORMAT.C
if errorlevel 1 goto test_fail
TESTS\M4\FORMAT.EXE > nul
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\TRY-FPR.FE -o TESTS\M4\TRY-FPR.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\M4\TRY-FPR.EXE TESTS\M4\TRY-FPR.C
if errorlevel 1 goto test_fail
TESTS\M4\TRY-FPR.EXE > nul
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\PROP.FE -o TESTS\M4\PROP.C > nul
if errorlevel 1 goto test_fail
wcl386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\M4\PROP.EXE TESTS\M4\PROPTEST.C
if errorlevel 1 goto test_fail
TESTS\M4\PROP.EXE > nul
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-ARI.FE -o TESTS\M4\BAD-ARI.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-VERB.FE -o TESTS\M4\BAD-VERB.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-RUN.FE -o TESTS\M4\BAD-RUN.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-TYPE.FE -o TESTS\M4\BAD-TYP.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-TRY.FE -o TESTS\M4\BAD-TRY.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-WRIT.FE -o TESTS\M4\BAD-WRI.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-BUFW.FE -o TESTS\M4\BAD-BUFW.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-MANY.FE -o TESTS\M4\BAD-MANY.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-OPEN.FE -o TESTS\M4\BAD-OPEN.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M4\BAD-CLS.FE -o TESTS\M4\BAD-CLS.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\DEFER.FE -o TESTS\M5\DEFER.C > nul
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\OWNED.FE -o TESTS\M5\OWNED.C > nul
if errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\BAD-MOVE.FE -o TESTS\M5\BAD-MOVE.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\BAD-DEST.FE -o TESTS\M5\BAD-DES.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\BAD-DROP.FE -o TESTS\M5\BAD-DROP.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\BAD-DBL.FE -o TESTS\M5\BAD-DBL.C > nul
if not errorlevel 1 goto test_fail
fec.exe --target=bits32 --emit-c TESTS\M5\BAD-COND.FE -o TESTS\M5\BAD-COND.C > nul
if not errorlevel 1 goto test_fail
if exist TESTS\M5\RUNTIME-G.C del TESTS\M5\RUNTIME-G.C
if exist TESTS\M5\RUNTIME.O del TESTS\M5\RUNTIME.O
if exist TESTS\M5\RUNTIME.EXE del TESTS\M5\RUNTIME.EXE
fec.exe --target=bits32 --emit-c TESTS\M5\RUNTIME.FE -o TESTS\M5\RUNTIME-G.C > nul
if errorlevel 1 goto test_fail
rem Compile generated source and the C89 runtime harness in one WCL386 invocation
rem so both objects use the same DOS/4GW startup and runtime library.
wcl386 -q -za -bt=dos -dmalloc=m5_malloc -dfree=m5_free -fe=TESTS\M5\RUNTIME.EXE TESTS\M5\RUNTIME-G.C TESTS\M5\RUNTIME.C
if errorlevel 1 goto test_fail
TESTS\M5\RUNTIME.EXE
if errorlevel 1 goto test_fail

echo OK>TEST.OK
cd C:\FEC
goto test_done

:test_fail
echo FAIL>TEST.FAIL
verify other 2>nul

:test_done
