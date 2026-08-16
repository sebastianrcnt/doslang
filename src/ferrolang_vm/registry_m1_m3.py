"""Explicit M1--M3 commands and expectations from TEST-DOS.BAT."""
from __future__ import annotations

from .suite import Case


def _c(milestone: int, name: str, command: str, ok: bool = True) -> Case:
    return Case(f"m{milestone}-{name}", milestone, command, ok)


M1_M3_CASES: list[Case] = []
for _name in ("basic", "literals", "keybuilt", "v012form"):
    M1_M3_CASES.append(_c(1, f"{_name}-parse", f"FEC.EXE --dump-ast TESTS\\PASS\\{_name.upper()}.FE"))
for _name in ("core", "fmt", "io", "list", "map", "mem", "str", "sys"):
    M1_M3_CASES.append(_c(1, f"std-{_name}-parse", f"FEC.EXE --dump-ast STD\\{_name.upper()}.FE"))
for _name in ("misssemi", "unclcomm", "logical"):
    M1_M3_CASES.append(_c(1, f"{_name}-reject", f"FEC.EXE --dump-ast TESTS\\FAIL\\{_name.upper()}.FE", False))

for _name in ("hello", "scopes"):
    _upper = _name.upper()
    M1_M3_CASES.extend([
        _c(2, f"{_name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M2\\{_upper}.FE -o TESTS\\M2\\{_upper}.C"),
        _c(2, f"{_name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M2\\{_upper}.EXE TESTS\\M2\\{_upper}.C"),
        _c(2, f"{_name}-run", f"TESTS\\M2\\{_upper}.EXE"),
    ])
M1_M3_CASES.extend([
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
    M1_M3_CASES.append(_c(2, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M2\\{_name.upper()}.FE -o TESTS\\M2\\{_output}.C", False))

def _m3_runtime(name: str) -> list[Case]:
    upper = name.upper()
    return [
        _c(3, f"{name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M3\\{upper}.FE -o TESTS\\M3\\{upper}.C"),
        _c(3, f"{name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M3\\{upper}.EXE TESTS\\M3\\{upper}.C"),
        _c(3, f"{name}-run", f"TESTS\\M3\\{upper}.EXE"),
    ]


for _name in ("struct", "enum", "array", "mutable"):
    M1_M3_CASES.extend(_m3_runtime(_name))
for _name in ("bad-mlet", "bad-shwr"):
    M1_M3_CASES.append(_c(3, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M3\\{_name.upper()}.FE -o TESTS\\M3\\{_name.upper()}.C", False))
for _name in ("str", "for", "nested", "char", "arrayctx"):
    M1_M3_CASES.extend(_m3_runtime(_name))
for _name in ("bounds", "slcbound"):
    _upper = _name.upper()
    M1_M3_CASES.extend([
        _c(3, f"{_name}-emit", f"FEC.EXE --target=bits32 --emit-c TESTS\\M3\\{_upper}.FE -o TESTS\\M3\\{_upper}.C"),
        _c(3, f"{_name}-build", f"WCL386 -q -za -bt=dos -fe=TESTS\\M3\\{_upper}.EXE TESTS\\M3\\{_upper}.C"),
        _c(3, f"{_name}-trap", f"TESTS\\M3\\{_upper}.EXE", False),
    ])
M1_M3_CASES.extend([
    _c(3, "bounds-no-checks-emit", "FEC.EXE --target=bits32 --no-checks --emit-c TESTS\\M3\\BOUNDS.FE -o TESTS\\M3\\BOUNDS-N.C"),
    _c(3, "bounds-no-checks-build", "WCL386 -q -za -bt=dos -fe=TESTS\\M3\\BOUNDS-N.EXE TESTS\\M3\\BOUNDS-N.C"),
])
for _name in ("badfld", "badmat", "badarr", "badcycle", "badstr", "badchar", "badfield", "badindex"):
    M1_M3_CASES.append(_c(3, f"{_name}-reject", "FEC.EXE --target=bits32 --emit-c "
                            f"TESTS\\M3\\{_name.upper()}.FE -o TESTS\\M3\\{_name.upper()}.C", False))
