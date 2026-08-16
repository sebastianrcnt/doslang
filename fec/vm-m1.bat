@echo off
rem D: is the read-only exchange volume. Stage everything before running DOS tools.
if not exist C:\FEC md C:\FEC
if not exist C:\FEC\SRC md C:\FEC\SRC
if not exist C:\FEC\STD md C:\FEC\STD
if not exist C:\FEC\TESTS md C:\FEC\TESTS
if not exist C:\FEC\TESTS\PASS md C:\FEC\TESTS\PASS
if not exist C:\FEC\TESTS\FAIL md C:\FEC\TESTS\FAIL
if not exist C:\FEC\TESTS\M2 md C:\FEC\TESTS\M2
if not exist C:\FEC\TESTS\M3 md C:\FEC\TESTS\M3
if not exist C:\FEC\TESTS\M4 md C:\FEC\TESTS\M4
if not exist C:\FEC\TESTS\M5 md C:\FEC\TESTS\M5
if exist C:\FEC\VM.FAIL del C:\FEC\VM.FAIL
if exist C:\FEC\STAGE.FAIL del C:\FEC\STAGE.FAIL

copy D:\FEC\BUILD-~1.BAT C:\FEC\BUILD.BAT > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TEST-DOS.BAT C:\FEC\TEST-DOS.BAT > nul
if errorlevel 1 goto stage_fail

copy D:\FEC\SRC\ARENA.C C:\FEC\SRC\ARENA.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\ARENA.H C:\FEC\SRC\ARENA.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\DIAG.C C:\FEC\SRC\DIAG.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\DIAG.H C:\FEC\SRC\DIAG.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\LEXER.C C:\FEC\SRC\LEXER.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\LEXER.H C:\FEC\SRC\LEXER.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\AST.C C:\FEC\SRC\AST.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\AST.H C:\FEC\SRC\AST.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\PARSER.C C:\FEC\SRC\PARSER.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\PARSER.H C:\FEC\SRC\PARSER.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\DRIVER.C C:\FEC\SRC\DRIVER.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\TYPES.C C:\FEC\SRC\TYPES.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\TYPES.H C:\FEC\SRC\TYPES.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\CHECK.C C:\FEC\SRC\CHECK.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\CHECK.H C:\FEC\SRC\CHECK.H > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\EMIT_C.C C:\FEC\SRC\EMIT_C.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\SRC\EMIT_C.H C:\FEC\SRC\EMIT_C.H > nul
if errorlevel 1 goto stage_fail

copy D:\FEC\STD\CORE.FE C:\FEC\STD\CORE.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\FMT.FE C:\FEC\STD\FMT.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\IO.FE C:\FEC\STD\IO.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\LIST.FE C:\FEC\STD\LIST.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\MAP.FE C:\FEC\STD\MAP.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\MEM.FE C:\FEC\STD\MEM.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\STR.FE C:\FEC\STD\STR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\STD\SYS.FE C:\FEC\STD\SYS.FE > nul
if errorlevel 1 goto stage_fail

copy D:\FEC\TESTS\PASS\BASIC.FE C:\FEC\TESTS\PASS\BASIC.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\PASS\LITERALS.FE C:\FEC\TESTS\PASS\LITERALS.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\PASS\KEYWOR~1.FE C:\FEC\TESTS\PASS\KEYWOR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\PASS\V012-F~1.FE C:\FEC\TESTS\PASS\V012-F.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\FAIL\MISSIN~1.FE C:\FEC\TESTS\FAIL\MISSIN.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\FAIL\UNCLOS~1.FE C:\FEC\TESTS\FAIL\UNCLOS.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\FAIL\LOGICA~1.FE C:\FEC\TESTS\FAIL\LOGICA.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\HELLO.FE C:\FEC\TESTS\M2\HELLO.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\SCOPES.FE C:\FEC\TESTS\M2\SCOPES.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-CO~1.FE C:\FEC\TESTS\M2\BAD-CO.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-CAST.FE C:\FEC\TESTS\M2\BAD-CA.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-ASSI~1.FE C:\FEC\TESTS\M2\BAD-AS.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-UN~1.FE C:\FEC\TESTS\M2\BAD-UN.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-UN~2.FE C:\FEC\TESTS\M2\BAD-UI.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-AR~1.FE C:\FEC\TESTS\M2\BAD-AR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-TY~1.FE C:\FEC\TESTS\M2\BAD-TY.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-RE~1.FE C:\FEC\TESTS\M2\BAD-RE.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\BAD-VOID.FE C:\FEC\TESTS\M2\BAD-VO.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M2\CAST-W~1.FE C:\FEC\TESTS\M2\CAST-W.FE > nul
if errorlevel 1 goto stage_fail

copy D:\FEC\TESTS\M3\STRUCT.FE C:\FEC\TESTS\M3\STRUCT.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\ENUM.FE C:\FEC\TESTS\M3\ENUM.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\ARRAY.FE C:\FEC\TESTS\M3\ARRAY.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\STR.FE C:\FEC\TESTS\M3\STR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\FOR.FE C:\FEC\TESTS\M3\FOR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\NESTED.FE C:\FEC\TESTS\M3\NESTED.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\CHAR.FE C:\FEC\TESTS\M3\CHAR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\ARRAYCTX.FE C:\FEC\TESTS\M3\ARRAYCTX.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BOUNDS.FE C:\FEC\TESTS\M3\BOUNDS.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADFLD.FE C:\FEC\TESTS\M3\BADFLD.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADMAT.FE C:\FEC\TESTS\M3\BADMAT.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADARR.FE C:\FEC\TESTS\M3\BADARR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADCYCLE.FE C:\FEC\TESTS\M3\BADCYCLE.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADSTR.FE C:\FEC\TESTS\M3\BADSTR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADCHAR.FE C:\FEC\TESTS\M3\BADCHAR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADFIELD.FE C:\FEC\TESTS\M3\BADFIELD.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M3\BADINDEX.FE C:\FEC\TESTS\M3\BADINDEX.FE > nul
if errorlevel 1 goto stage_fail

copy D:\FEC\TESTS\M4\FORMAT.FE C:\FEC\TESTS\M4\FORMAT.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-ARI~1.FE C:\FEC\TESTS\M4\BAD-ARI.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-VERB.FE C:\FEC\TESTS\M4\BAD-VERB.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-RUN~1.FE C:\FEC\TESTS\M4\BAD-RUN.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-TYP~1.FE C:\FEC\TESTS\M4\BAD-TYP.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-TRY.FE C:\FEC\TESTS\M4\BAD-TRY.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\TRY-FPR~1.FE C:\FEC\TESTS\M4\TRY-FPR.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-WRI~1.FE C:\FEC\TESTS\M4\BAD-WRI.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\PROP.FE C:\FEC\TESTS\M4\PROP.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\PROPTEST.C C:\FEC\TESTS\M4\PROPTEST.C > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-MANY.FE C:\FEC\TESTS\M4\BAD-MANY.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-OPEN.FE C:\FEC\TESTS\M4\BAD-OPEN.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M4\BAD-CLS.FE C:\FEC\TESTS\M4\BAD-CLS.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\DEFER.FE C:\FEC\TESTS\M5\DEFER.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\OWNED.FE C:\FEC\TESTS\M5\OWNED.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\BAD-MOVE.FE C:\FEC\TESTS\M5\BAD-MOVE.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\BAD-DES~1.FE C:\FEC\TESTS\M5\BAD-DES.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\RUNTIME.FE C:\FEC\TESTS\M5\RUNTIME.FE > nul
if errorlevel 1 goto stage_fail
copy D:\FEC\TESTS\M5\RUNTIME.C C:\FEC\TESTS\M5\RUNTIME.C > nul
if errorlevel 1 goto stage_fail

call C:\FEC\TEST-DOS.BAT
if exist C:\FEC\TEST.OK goto vm_success
echo FAIL>C:\FEC\VM.FAIL
verify other 2>nul
goto stage_done

:vm_success
cd C:\FEC
goto stage_done

:stage_fail
echo FAIL>C:\FEC\STAGE.FAIL
echo FAIL>C:\FEC\VM.FAIL
verify other 2>nul

:stage_done
