@echo off
rem FreeDOS smoke tests. All work happens on the writable C: drive.
C:
cd \FEC
if exist TEST.OK del TEST.OK
if exist TEST.FAIL del TEST.FAIL
call C:\FEC\BUILD.BAT
if not exist BUILD.OK goto test_fail

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

echo OK>TEST.OK
cd C:\FEC
goto test_done

:test_fail
echo FAIL>TEST.FAIL
verify other 2>nul

:test_done
