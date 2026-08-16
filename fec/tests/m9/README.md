# M9 fixtures

M9 adds comptime type parameters and monomorphization.

`ok*.fe` must compile; `bad*.fe` must fail according to the first-line marker.
`defscope/` and `okscope/` are multi-unit definition-scope tests and require M8 imports. `okscope/` verifies that an exported generic may use its definition unit's private helper through `.fei` support metadata.

Coverage:
- generic functions and structs,
- multiple and duplicate instantiations,
- type aliases as comptime type values,
- `T == U` and `@is_int(T)` in comptime,
- instantiation-time operation errors,
- arity/type-argument errors,
- runtime `type` values forbidden,
- definition-unit name lookup,
- recursive instantiation depth limit.
- rejection of user value generics and generic type inference,
- same-instance recursive request reuse and selected-out `comptime if` semantic skipping.

M9's DOS gate should additionally inspect generated `fe_generics.c`: `okdedup.fe` must emit one body for the repeated `(id, i32)` instantiation.
