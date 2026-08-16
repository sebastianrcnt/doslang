"""Milestone case registry: DOS commands and their expected exit status.

Only commands live here; the ``.fe`` fixtures stay under ``fec/tests`` and are
copied into the disposable DOS filesystem by the runner. Case order is load
bearing -- ``emit`` must precede ``build`` must precede ``run`` for the same
fixture, because each step consumes the previous step's output.
"""
from __future__ import annotations

from .suite import Case

PASS = "TESTS\\PASS"
FAIL = "TESTS\\FAIL"
STD = "STD"
M2 = "TESTS\\M2"
M3 = "TESTS\\M3"
M4 = "TESTS\\M4"
M5 = "TESTS\\M5"
M6 = "TESTS\\M6"
OUT = "OUT"

# Emitted-C basenames that were hand-shortened for DOS 8.3. Keyed by milestone
# because the same fixture name maps to different outputs across milestones
# (``bad-type`` is BAD-TY in M2 but BAD-TYP in M4). The shortenings are not
# consistent -- M2 cut to six characters, M4 to seven, and several were never
# required at all since BAD-COND is already a legal 8.3 name. Preserved verbatim;
# changing one renames a file inside the DOS run, so re-verify if you touch it.
_OUT83 = {
    (2, "bad-cond"): "BAD-CO",
    (2, "bad-cast"): "BAD-CA",
    (2, "bad-asgn"): "BAD-AS",
    (2, "bad-unk"): "BAD-UN",
    (2, "bad-ari"): "BAD-AR",
    (2, "bad-type"): "BAD-TY",
    (2, "bad-ret"): "BAD-RE",
    (2, "bad-unit"): "BAD-UI",
    (2, "bad-void"): "BAD-VO",
    (4, "bad-type"): "BAD-TYP",
    (4, "bad-writ"): "BAD-WRI",
    (5, "bad-dest"): "BAD-DES",
}


def _case(milestone: int, name: str, command: str, ok: bool = True) -> Case:
    return Case(f"m{milestone}-{name}", milestone, command, ok)


def _fe(directory: str, name: str) -> str:
    return f"{directory}\\{name.upper()}.FE"


def _emit(source: str, output: str, *, target: str = "bits32",
          flags: tuple[str, ...] = (), output_first: bool = False) -> str:
    """``fec`` invocation that translates ``source`` to C at ``output``.

    ``output_first`` reproduces the M6 cases, which pass ``-o`` before the input
    file while every other milestone passes it after.
    """
    parts = ["FEC.EXE", f"--target={target}", *flags, "--emit-c"]
    parts += ["-o", output, source] if output_first else [source, "-o", output]
    return " ".join(parts)


def _wcl(exe: str, *sources: str, bits: int = 32, strict: bool = False,
         defines: tuple[str, ...] = ()) -> str:
    """Open Watcom invocation. ``strict`` is the M4 ``-wx -wcd=202`` pairing:
    warnings are errors except W202, which the generated C trips on unused
    helpers (see AGENTS.md)."""
    parts = ["WCL386" if bits == 32 else "WCL", "-q", "-za"]
    if strict:
        parts += ["-wx", "-wcd=202"]
    parts += ["-bt=dos", *defines, f"-fe={exe}", *sources]
    return " ".join(parts)


def _dump_ast(milestone: int, directory: str, names: tuple[str, ...], *,
              suffix: str, ok: bool = True, prefix: str = "") -> list[Case]:
    return [
        _case(milestone, f"{prefix}{name}-{suffix}",
              f"FEC.EXE --dump-ast {_fe(directory, name)}", ok)
        for name in names
    ]


def _rejects(milestone: int, directory: str, names: tuple[str, ...], *,
             suffix: str = "") -> list[Case]:
    """Fixtures that must fail to compile. The emitted-C path is still spelled
    out because ``fec`` needs an ``-o`` even when it is expected to bail."""
    return [
        _case(milestone, f"{name}-{suffix}" if suffix else name,
              _emit(_fe(directory, name),
                    f"{directory}\\{_OUT83.get((milestone, name), name.upper())}.C"),
              False)
        for name in names
    ]


def _triple(milestone: int, name: str, directory: str, *, stem: str | None = None,
            target: str = "bits32", bits: int = 32, strict: bool = False,
            build_source: str | None = None, emit_suffix: str | None = "emit",
            run_suffix: str = "run", run_ok: bool = True) -> list[Case]:
    """emit -> build -> run for one fixture.

    ``stem`` renames the C/EXE pair when the fixture name does not fit 8.3 or
    collides (M2 castwhil emits CAST16). ``build_source`` compiles a different
    file than the one emitted (M4 prop emits PROP.C but builds PROPTEST.C, which
    ``#include``s it).
    """
    stem = stem or name.upper()
    cfile = f"{directory}\\{stem}.C"
    exe = f"{directory}\\{stem}.EXE"
    emit_id = f"{name}-{emit_suffix}" if emit_suffix else name
    return [
        _case(milestone, emit_id, _emit(_fe(directory, name), cfile, target=target)),
        _case(milestone, f"{name}-build",
              _wcl(exe, build_source or cfile, bits=bits, strict=strict)),
        _case(milestone, f"{name}-{run_suffix}", exe, run_ok),
    ]


CASES: list[Case] = [
    # -- M1: parse only -------------------------------------------------------
    *_dump_ast(1, PASS, ("basic", "literals", "keybuilt", "v012form"), suffix="parse"),
    *_dump_ast(1, STD, ("core", "fmt", "io", "list", "map", "mem", "str", "sys"),
               suffix="parse", prefix="std-"),
    *_dump_ast(1, FAIL, ("misssemi", "unclcomm", "logical"), suffix="reject", ok=False),

    # -- M2: first generated C ------------------------------------------------
    *_triple(2, "hello", M2),
    *_triple(2, "scopes", M2),
    *_triple(2, "castwhil", M2, stem="CAST16", target="bits16", bits=16),
    *_rejects(2, M2, ("bad-cond", "bad-cast", "bad-asgn", "bad-unk", "bad-ari",
                      "bad-type", "bad-ret", "bad-unit", "bad-void"), suffix="reject"),

    # -- M3: aggregates, strings, bounds checks -------------------------------
    *_triple(3, "struct", M3),
    *_triple(3, "enum", M3),
    *_triple(3, "array", M3),
    *_triple(3, "mutable", M3),
    *_rejects(3, M3, ("bad-mlet", "bad-shwr"), suffix="reject"),
    *_triple(3, "str", M3),
    *_triple(3, "for", M3),
    *_triple(3, "nested", M3),
    *_triple(3, "char", M3),
    *_triple(3, "arrayctx", M3),
    # These two must trap at runtime: the bounds check is the feature under test.
    *_triple(3, "bounds", M3, run_suffix="trap", run_ok=False),
    *_triple(3, "slcbound", M3, run_suffix="trap", run_ok=False),
    # --no-checks is proved by a differential on one source. NOCHK.FE reads one
    # element past a [2]i32 and returns x - x, which is 0 whatever garbage the
    # unchecked read produced: compiled with checks it must trap, compiled with
    # --no-checks it must run to completion. BOUNDS.FE cannot serve as the
    # unchecked half because it returns the out-of-bounds value directly, so its
    # exit code would be whatever happens to sit past the array on the stack.
    *_triple(3, "nochk", M3, run_suffix="trap", run_ok=False),
    _case(3, "nochk-off-emit",
          _emit(_fe(M3, "nochk"), f"{M3}\\NOCHK-N.C", flags=("--no-checks",))),
    _case(3, "nochk-off-build",
          _wcl(f"{M3}\\NOCHK-N.EXE", f"{M3}\\NOCHK-N.C")),
    _case(3, "nochk-off-run", f"{M3}\\NOCHK-N.EXE"),
    *_rejects(3, M3, ("badfld", "badmat", "badarr", "badcycle", "badstr", "badchar",
                      "badfield", "badindex"), suffix="reject"),

    # -- M4: formatting and error propagation ---------------------------------
    *_triple(4, "format", M4, strict=True, emit_suffix=None),
    *_triple(4, "try-fpr", M4, strict=True, emit_suffix=None),
    *_triple(4, "prop", M4, strict=True, emit_suffix=None,
             build_source=f"{M4}\\PROPTEST.C"),
    *_rejects(4, M4, ("bad-ari", "bad-verb", "bad-run", "bad-type", "bad-try",
                      "bad-writ", "bad-bufw", "bad-many", "bad-open", "bad-cls")),

    # -- M5: defer and ownership ----------------------------------------------
    _case(5, "defer", _emit(_fe(M5, "defer"), f"{M5}\\DEFER.C")),
    _case(5, "owned", _emit(_fe(M5, "owned"), f"{M5}\\OWNED.C")),
    *_rejects(5, M5, ("bad-move", "bad-dest", "bad-drop", "bad-dbl", "bad-cond",
                      "bad-proj", "bad-clos", "bad-loop")),
    # The runtime case links the generated C against a hand-written allocator
    # shim, so malloc/free are redirected at compile time.
    _case(5, "runtime", _emit(_fe(M5, "runtime"), f"{M5}\\RUNT-G.C")),
    _case(5, "runtime-build",
          _wcl(f"{M5}\\RUNTIME.EXE", f"{M5}\\RUNT-G.C", f"{M5}\\RUNTIME.C",
               defines=("-dmalloc=m5_malloc", "-dfree=m5_free"))),
    _case(5, "runtime-run", f"{M5}\\RUNTIME.EXE"),

    # -- M6: borrow checking (R1--R8) -----------------------------------------
    *[_case(6, name, f"FEC.EXE --check {_fe(M6, name)}", False) for name in (
        "badarg", "badbinit", "badbrmov", "baddefer", "badfld", "badglob", "badgmut",
        "badinv", "badlocsl", "badloop", "badmove", "badmut", "badmut2", "badptr",
        "badret", "badrfld", "badridx", "badscop", "badself", "badshwr", "badslfld",
        "badtwo", "badup", "badweak")],
    *[_case(6, name, _emit(_fe(M6, name), f"{OUT}\\{name.upper()}.C",
                           output_first=True)) for name in (
        "okbranch", "okdefer", "okglobcp", "oklast", "okr8free", "okr8join",
        "okr8meth", "okr8stat", "okrebor", "okrtlast", "okshare", "okslreb",
        "okstatic", "oktemp", "oktrim", "okwcall")],
]

MAX_MILESTONE: int = max(case.milestone for case in CASES)
MILESTONES: tuple[str, ...] = tuple(f"m{number}"
                                    for number in range(1, MAX_MILESTONE + 1))


def milestone_number(name: str) -> int:
    """Parse an ``mN`` selector against the milestones the registry knows about."""
    if not name.startswith("m") or not name[1:].isdigit():
        raise ValueError(f"invalid milestone: {name}")
    value = int(name[1:])
    if value not in range(1, MAX_MILESTONE + 1):
        raise ValueError(f"unsupported milestone: {name}")
    return value


def all_cases(*, through: int = MAX_MILESTONE, only: int | None = None) -> list[Case]:
    if only is not None:
        return [case for case in CASES if case.milestone == only]
    return [case for case in CASES if case.milestone <= through]
