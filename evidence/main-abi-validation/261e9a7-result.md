# Public C ABI regression validation (261e9a7)

Product branch: `xuanwo/main-abi-layout-tests`
Product head: `261e9a70a61cfabb587bb4f3e461fcb6a3ecfae2`
Product tree: `1840ff99f0a98bdb43295a3b36a48c966b1168b2`
Base: `9d7fda2c9513a468aafb762c16e1aa0e6dc56309` (tree `4e0065a40020590d6f3d6fd4bc6f36b0e65b88f5`)

## Change

`git diff --stat 9d7fda2..261e9a7`:

```
 tests/compile_and_run_test.rs |    9 +
 tests/support/abi_layout.rs   | 2103 +++++++++++++++++++++++++++++++++++++++++
 2 files changed, 2112 insertions(+)
```

The test is an `#[ignore]` test in `tests/compile_and_run_test.rs`, which the
existing Linux and macOS CI steps run with:

```
cargo test --test compile_and_run_test -- --ignored
```

No workflow change is needed.

## Inventory covered

The explicit association table covers the complete current public header:

- 101 complete public structs with 539 fields: size, alignment, and every field offset.
- 35 typed C enums with 134 enumerators: size, alignment, and every value by
  explicit `C enumerator -> Rust variant` name association.
- 9 raw-i32 constant families with 29 names: constants only, not represented as
  Rust enums.
- 3 Arrow C Data Interface structs (`struct ArrowSchema`, `struct ArrowArray`,
  `struct ArrowArrayStream`): size and alignment only because their fields are
  private in arrow-rs.
- 75 pointer-only opaque handles: excluded from layout checks and checked for
  completeness as opaque handles.
- Header scan total: 44 enum families, 163 enumerator names, 101 concrete
  structs, 3 plain Arrow structs, 75 opaque handles.
- Probe comparison total: 980 semantic records.

The header scan fails on new, renamed, stale, or unmapped symbols. It never
pairs symbols by numeric value.

## GREEN evidence

Workdir: `/home/xuanwo/Code/executions/lance-c/main-abi-261e9a7`
Evidence: `/home/xuanwo/Code/evidence/lance-c/main-abi-261e9a7/green`
Result: `PASS` at `2026-11-13T08:31:38+01:00`

```
01-fmt PASS
02-check PASS
03-clippy PASS
04-scanner PASS
05-abi PASS
06-c-consumer PASS
07-cpp-consumer PASS
```

The ABI test passed with `1 passed; 0 failed`.

## Negative controls

Workdir: `/home/xuanwo/Code/executions/lance-c/main-abi-261e9a7`
Evidence: `/home/xuanwo/Code/evidence/lance-c/main-abi-261e9a7/controls-rerun`
Result: `PASS` at `2026-11-13T08:38:27+01:00`, source reverted and clean,
then `08-abi-after-controls PASS`.

All controls failed for the intended semantic diagnostic:

- `LANCE_ERR_PANIC` 9 -> 10: `enum-value|LanceErrorCode|LANCE_ERR_PANIC|9`
  versus `...|10`.
- Swap `LANCE_ERR_IO` and `LANCE_ERR_NOT_FOUND`: both names reported with
  swapped values 2/3.
- Swap `LanceSessionCacheStats::index_cache_hits` and
  `index_cache_misses`: offsets reported as 0/8 versus 8/0.
- Rust `LanceErrorCode` `repr(C)` -> `repr(u64)`: enum size reported as 8
  versus C size 4.
- Add an unmapped `LANCE_ERR_NEW`: scanner reports
  `enumerator \`LanceErrorCode::LANCE_ERR_NEW\` is in the public header but not in the ABI manifest`.

Each control runs the focused ABI test and requires failure plus the expected
diagnostic. Patches are retained in `controls-rerun/control-patches/`.

## Limitations

- Pointer-only opaque internals are intentionally not laid out; only their
  existence is checked.
- Arrow C Data Interface structs are compared by size and alignment, not field
  offsets, because arrow-rs keeps those fields private.
- Raw-i32 constant families have no Rust enum, so they are checked as named
  constants rather than enum layout.
- The compiled comparison is native-only, matching the supported test hosts.
- This delivers the independent current-main ABI regression. It does not
  complete issue #102: author coordination and the async Completion/bridge
  integration remain separate work.
