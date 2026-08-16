# M8 fixtures

M8 is the first multi-unit milestone, so cases live in subdirectories. Each case is compiled separately with that directory on the import path.

Pass cases:
- `basic`: public function across units.
- `pubfld`: public type and public field across units.
- `dotted` and `alias`: canonical dotted unit paths, last-segment binding, and explicit import aliases.
- `dotpriv`: a dotted unit prefix does not grant access to another unit's private declarations.
- `errsame`: two units use the same anonymous `error.Name`; the driver must assign one deterministic `core.Error` code.
- `errdet`: the same anonymous error-name set appears in a different source/import order; generated `fe_errors.h` must be byte-identical to `errsame` modulo the intentionally different unit graph.

Fail cases:
- `privfn`: private declaration access.
- `privfld`: private field access.
- `missing`: unresolved import.
- `cycle`: cyclic imports.
- `unitbad`: filename/unit-name mismatch.
- `badupper` and `badlong`: DOS-safe lowercase, eight-character unit-segment limit.
- `bindconf`: two imports with the same last-segment binding require an alias.
- `pubpriv`: a public signature cannot expose a private nominal type.
- `errnom`: nominal error cannot flow into `core.Error` via `try`.

The current M5 `TEST-DOS.BAT` is intentionally unchanged. M8 should add procedural checks for `.fei` creation/hash invalidation and deterministic `fe_errors.h` using these fixtures.

The M8 DOS gate must additionally construct two separate `-I` roots containing the same canonical unit and require an ambiguity error; repeat the case with two paths to the same canonical file and require deduplication. It must also verify that `std.*` resolves only from the built-in std root, ordinary user units never do, and that changing a private non-generic implementation preserves the dependent interface-cache hit. Those checks need temporary roots/cache inspection and deliberately remain procedural rather than encoding host paths in fixtures.
