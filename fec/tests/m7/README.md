# M7 fixtures

M7 adds optionals and error unions on top of the M6 ownership model.

`ok*.fe` must compile. `bad*.fe` must fail according to the first-line error marker.
The files are not wired into `TEST-DOS.BAT` until M7 work begins.

Coverage: `?T`, `.?`, `Some`/`None` pattern-only destructuring, `mem.replace` extraction, `orelse`, nominal error unions, `try`, block/short `catch`, error code zero rejection, and R4/R7 interactions with optional references/owners.
