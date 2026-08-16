# M8 fixtures

M8 is the first multi-unit milestone, so cases live in subdirectories. Each case is compiled separately with that directory on the import path.

Pass cases:
- `basic`: public function across units.
- `pubfld`: public type and public field across units.
- `errsame`: two units use the same anonymous `error.Name`; the driver must assign one deterministic `core.Error` code.
- `errdet`: the same anonymous error-name set appears in a different source/import order; generated `fe_errors.h` must be byte-identical to `errsame` modulo the intentionally different unit graph.

Fail cases:
- `privfn`: private declaration access.
- `privfld`: private field access.
- `missing`: unresolved import.
- `cycle`: cyclic imports.
- `unitbad`: filename/unit-name mismatch.
- `errnom`: nominal error cannot flow into `core.Error` via `try`.

The current M5 `TEST-DOS.BAT` is intentionally unchanged. M8 should add procedural checks for `.fei` creation/hash invalidation and deterministic `fe_errors.h` using these fixtures.
