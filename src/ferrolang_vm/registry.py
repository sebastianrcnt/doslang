"""Explicit M1--M3 commands and expectations from TEST-DOS.BAT."""
from __future__ import annotations

from .suite import Case


def _c(milestone: int, name: str, command: str, ok: bool = True) -> Case:
    return Case(f"m{milestone}-{name}", milestone, command, ok)


CASES: list[Case] = []
for _name in ("basic", "literals", "keybuilt", "v012form"):
    CASES.append(_c(1, f"{_name}-parse", f"FEC.EXE --dump-ast TESTS\\PASS\\{_name.upper()}.FE"))
for _name in ("core", "fmt", "io", "list", "map", "mem", "str", "sys"):
    CASES.append(_c(1, f"std-{_name}-parse", f"FEC.EXE --dump-ast STD\\{_name.upper()}.FE"))
for _name in ("misssemi", "unclcomm", "logical"):
    CASES.append(_c(1, f"{_name}-reject", f"FEC.EXE --dump-ast TESTS\\FAIL\\{_name.upper()}.FE", False))

for _name in ("hello", "scopes"):
    _upper = _name.upper()
    CASES.extend([
        _c(2, f"{_name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M2\\{_upper}.FE -o TESTS\\M2\\{_upper}.C"),
        _c(2, f"{_name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M2\\{_upper}.EXE TESTS\\M2\\{_upper}.C"),
        _c(2, f"{_name}-run", f"TESTS\\M2\\{_upper}.EXE"),
    ])
CASES.extend([
    _c(2, "castwhil-emit", "FEC.EXE --target=bits16 --emit-c TESTS\\M2\\CASTWHIL.FE -o TESTS\\M2\\CAST16.C"),
    _c(2, "castwhil-build", "WCL -q -za -bt=dos -fe=TESTS\\M2\\CAST16.EXE TESTS\\M2\\CAST16.C"),
    _c(2, "castwhil-run", "TESTS\\M2\\CAST16.EXE"),
])
_m2_outputs = {
    "bad-cond": "BAD-CO", "bad-cast": "BAD-CA", "bad-asgn": "BAD-AS",
    "bad-unk": "BAD-UN", "bad-ari": "BAD-AR", "bad-type": "BAD-TY",
    "bad-ret": "BAD-RE", "bad-unit": "BAD-UI", "bad-void": "BAD-VO",
}
for _name, _output in _m2_outputs.items():
    CASES.append(_c(2, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M2\\{_name.upper()}.FE -o TESTS\\M2\\{_output}.C", False))

def _m3_runtime(name: str) -> list[Case]:
    upper = name.upper()
    return [
        _c(3, f"{name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M3\\{upper}.FE -o TESTS\\M3\\{upper}.C"),
        _c(3, f"{name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M3\\{upper}.EXE TESTS\\M3\\{upper}.C"),
        _c(3, f"{name}-run", f"TESTS\\M3\\{upper}.EXE"),
    ]


for _name in ("struct", "enum", "array", "mutable"):
    CASES.extend(_m3_runtime(_name))
for _name in ("bad-mlet", "bad-shwr"):
    CASES.append(_c(3, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M3\\{_name.upper()}.FE -o TESTS\\M3\\{_name.upper()}.C", False))
for _name in ("str", "for", "nested", "char", "arrayctx"):
    CASES.extend(_m3_runtime(_name))
for _name in ("bounds", "slcbound"):
    _upper = _name.upper()
    CASES.extend([
        _c(3, f"{_name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M3\\{_upper}.FE -o TESTS\\M3\\{_upper}.C"),
        _c(3, f"{_name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M3\\{_upper}.EXE TESTS\\M3\\{_upper}.C"),
        _c(3, f"{_name}-trap", f"TESTS\\M3\\{_upper}.EXE", False),
    ])
CASES.extend([
    _c(3, "bounds-no-checks-emit", "FEC.EXE --target=bits32 --no-checks --emit-c TESTS\\M3\\BOUNDS.FE -o TESTS\\M3\\BOUNDS-N.C"),
    _c(3, "bounds-no-checks-build", "WCL386 -q -za -bt=dos -fe=TESTS\\M3\\BOUNDS-N.EXE TESTS\\M3\\BOUNDS-N.C"),
])
for _name in ("badfld", "badmat", "badarr", "badcycle", "badstr", "badchar", "badfield", "badindex"):
    CASES.append(_c(3, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M3\\{_name.upper()}.FE -o TESTS\\M3\\{_name.upper()}.C", False))

"""Explicit M4--M5 cases from TEST-DOS.BAT and M6 fixture expectations.

Only commands and their expected status live here; the ``.fe`` fixtures stay
under ``fec/tests`` and are copied by the runner.
"""
def _c(milestone: int, name: str, command: str, ok: bool) -> Case:
    return Case(f"m{milestone}-{name}", milestone, command, ok)


CASES.extend([
    _c(4, "format", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\FORMAT.FE -o TESTS\\M4\\FORMAT.C", True),
    _c(4, "format-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\FORMAT.EXE TESTS\\M4\\FORMAT.C", True),
    _c(4, "format-run", "TESTS\\M4\\FORMAT.EXE", True),
    _c(4, "try-fpr", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\TRY-FPR.FE -o TESTS\\M4\\TRY-FPR.C", True),
    _c(4, "try-fpr-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\TRY-FPR.EXE TESTS\\M4\\TRY-FPR.C", True),
    _c(4, "try-fpr-run", "TESTS\\M4\\TRY-FPR.EXE", True),
    _c(4, "prop", "FEC.EXE --target=bits32 --emit-c TESTS\\M4\\PROP.FE -o TESTS\\M4\\PROP.C", True),
    _c(4, "prop-build", "WCL386 -q -za -wx -wcd=202 -bt=dos -fe=TESTS\\M4\\PROP.EXE TESTS\\M4\\PROPTEST.C", True),
    _c(4, "prop-run", "TESTS\\M4\\PROP.EXE", True),
])

_m4_outputs = {"bad-type": "BAD-TYP", "bad-writ": "BAD-WRI"}
for _name in ("bad-ari", "bad-verb", "bad-run", "bad-type", "bad-try", "bad-writ", "bad-bufw", "bad-many", "bad-open", "bad-cls"):
    _output = _m4_outputs.get(_name, _name.upper())
    CASES.append(_c(4, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M4\\{_name.upper()}.FE -o TESTS\\M4\\{_output}.C", False))

for _name in ("defer", "owned"):
    CASES.append(_c(5, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M5\\{_name.upper()}.FE -o TESTS\\M5\\{_name.upper()}.C", True))
for _name in ("bad-move", "bad-dest", "bad-drop", "bad-dbl", "bad-cond", "bad-proj", "bad-clos", "bad-loop"):
    _output = "BAD-DES" if _name == "bad-dest" else _name.upper()
    CASES.append(_c(5, _name, "FEC.EXE --target=bits32 --emit-c "
                           f"TESTS\\M5\\{_name.upper()}.FE -o TESTS\\M5\\{_output}.C", False))
CASES += [
    _c(5, "runtime", "FEC.EXE --target=bits32 --emit-c TESTS\\M5\\RUNTIME.FE -o TESTS\\M5\\RUNT-G.C", True),
    _c(5, "runtime-build", "WCL386 -q -za -bt=dos -dmalloc=m5_malloc -dfree=m5_free -fe=TESTS\\M5\\RUNTIME.EXE TESTS\\M5\\RUNT-G.C TESTS\\M5\\RUNTIME.C", True),
    _c(5, "runtime-run", "TESTS\\M5\\RUNTIME.EXE", True),
]

for _name in ("badarg", "badbinit", "badbrmov", "baddefer", "badfld", "badglob", "badgmut", "badinv", "badlocsl", "badloop", "badmove", "badmut", "badmut2", "badptr", "badret", "badrfld", "badridx", "badscop", "badself", "badshwr", "badslfld", "badtwo", "badup", "badweak"):
    CASES.append(_c(6, _name, f"FEC.EXE --check TESTS\\M6\\{_name.upper()}.FE", False))
for _name in ("okbranch", "okdefer", "okglobcp", "oklast", "okr8free", "okr8join", "okr8meth", "okr8stat", "okrebor", "okrtlast", "okshare", "okslreb", "okstatic", "oktemp", "oktrim", "okwcall"):
    CASES.append(_c(6, _name, f"FEC.EXE --target=bits32 --emit-c -o OUT\\{_name.upper()}.C TESTS\\M6\\{_name.upper()}.FE", True))


def all_cases(*, through: int = 6, only: int | None = None) -> list[Case]:
    if only is not None:
        return [case for case in CASES if case.milestone == only]
    return [case for case in CASES if case.milestone <= through]
