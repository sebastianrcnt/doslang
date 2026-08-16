# M7 fixtures

M7 adds optionals and error unions on top of the M6 ownership model.

`ok*.fe` must compile. `bad*.fe` must fail according to the first-line error marker.
The branch-level `m7-smoke` workflow builds the compiler as C89 and checks every M7 pass/fail fixture. Formal regression remains the FreeDOS/Open Watcom path.

Coverage: contextual `null`, `?T`, `.?`, `Some`/`None` pattern-only non-destructive views, `mem.replace` extraction, lazy `orelse`/`catch`, nominal error unions, `try`, block/short `catch`, and error code/name uniqueness, plus R4/R7 interactions with optional references/owners.
