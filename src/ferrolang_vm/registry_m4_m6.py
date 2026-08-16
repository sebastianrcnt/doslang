"""Explicit M4--M5 cases from TEST-DOS.BAT and M6 fixture expectations.

Only commands and their expected status live here; the ``.fe`` fixtures stay
under ``fec/tests`` and are copied by the runner.
"""
from __future__ import annotations

from .suite import Case


def _c(milestone: int, name: str, command: str, ok: bool) -> Case:
    return Case(f"m{milestone}-{name}", milestone, command, ok)


M4_M6_CASES: list[Case] = [
    _c(4, "format", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\FORMAT.FE -o TESTS\\M4\\FORMAT.C", True),
    _c(4, "format-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\FORMAT.EXE TESTS\\M4\\FORMAT.C", True),
    _c(4, "format-run", "TESTS\\M4\\FORMAT.EXE", True),
    _c(4, "try-fpr", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\TRY-FPR.FE -o TESTS\\M4\\TRY-FPR.C", True),
    _c(4, "try-fpr-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\TRY-FPR.EXE TESTS\\M4\\TRY-FPR.C", True),
    _c(4, "try-fpr-run", "TESTS\\M4\\TRY-FPR.EXE", True),
    _c(4, "prop", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\PROP.FE -o TESTS\\M4\\PROP.C", True),
    _c(4, "prop-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\PROP.EXE TESTS\\M4\\PROPTEST.C", True),
    _c(4, "prop-run", "TESTS\\M4\\PROP.EXE", True),
]

_m4_outputs = {"bad-type": "BAD-TYP", "bad-writ": "BAD-WRI"}
for _name in ("bad-ari", "bad-verb", "bad-run", "bad-type", "bad-try", "bad-writ", "bad-bufw", "bad-many", "bad-open", "bad-cls"):
    _output = _m4_outputs.get(_name, _name.upper())
    M4_M6_CASES.append(_c(4, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M4\\{_name.upper()}.FE -o TESTS\\M4\\{_output}.C", False))

for _name in ("defer", "owned"):
    M4_M6_CASES.append(_c(5, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M5\\{_name.upper()}.FE -o TESTS\\M5\\{_name.upper()}.C", True))
for _name in ("bad-move", "bad-dest", "bad-drop", "bad-dbl", "bad-cond", "bad-proj", "bad-clos", "bad-loop"):
    _output = "BAD-DES" if _name == "bad-dest" else _name.upper()
    M4_M6_CASES.append(_c(5, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M5\\{_name.upper()}.FE -o TESTS\\M5\\{_output}.C", False))
M4_M6_CASES += [
    _c(5, "runtime", "FEC.EXE --target=bits32 --emit-c TESTS\\M5\\RUNTIME.FE -o TESTS\\M5\\RUNT-G.C", True),
    _c(5, "runtime-build", "WCL386 -q -za -bt=dos -dmalloc=m5_malloc -dfree=m5_free -fe=TESTS\\M5\\RUNTIME.EXE TESTS\\M5\\RUNT-G.C TESTS\\M5\\RUNTIME.C", True),
    _c(5, "runtime-run", "TESTS\\M5\\RUNTIME.EXE", True),
]

for _name in ("badarg", "badbinit", "badbrmov", "baddefer", "badfld", "badglob", "badgmut", "badinv", "badlocsl", "badloop", "badmove", "badmut", "badmut2", "badptr", "badret", "badrfld", "badridx", "badscop", "badself", "badshwr", "badslfld", "badtwo", "badup", "badweak"):
    M4_M6_CASES.append(_c(6, _name, f"FEC.EXE --check TESTS\\M6\\{_name.upper()}.FE", False))
for _name in ("okbranch", "okdefer", "okglobcp", "oklast", "okr8free", "okr8join", "okr8meth", "okr8stat", "okrebor", "okrtlast", "okshare", "okslreb", "okstatic", "oktemp", "oktrim", "okwcall"):
    M4_M6_CASES.append(_c(6, _name, f"FEC.EXE --target=bits32 --emit-c -o OUT\\{_name.upper()}.C TESTS\\M6\\{_name.upper()}.FE", True))
