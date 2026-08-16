# M6 fixtures

These fixtures pin the M6 ownership/borrow rules before implementation.

- `ok*.fe` must compile with `--target=bits32 --emit-c`.
- `bad*.fe` must fail compilation; the first line follows the `// ERROR:<line>:<substring>` convention from SPEC §12.
- They are registered in `src/ferrolang_vm/registry.py`, which is what decides whether a milestone runs.
- When M6 starts, wire this directory into the DOS/QEMU gate without changing the expected result of any fixture.
- M6 also owns the general-global borrow restriction from R10 because AGENTS.md explicitly groups that change with the `own.c` state-machine work.

Coverage: R4 storage restrictions, R5 scope, R6 shared/exclusive liveness, root-local field/index conflicts, call-only `&mut -> &` reborrows, branch/loop `MaybeMoved` and initialization-state merging, R7 invalidation, R8 derived-return provenance joins, defer lifetime extension, and global borrow restrictions.
