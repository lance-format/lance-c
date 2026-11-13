# Native strict-before checkout candidate

This directory contains a validation-only candidate for lance-c #108. It adds
one native API and its focused tests on top of immutable Lance 14.0.2. It does
not change the C ABI, C implementation, dependency pins, error numbers, MSRV,
or Python code.

## Base

- Repository: `lance-format/lance`
- Commit: `40f15ed1b71b66950961b175829fdf17a763f108` (Lance 14.0.2)
- Patch: `native-checkout-before.patch`
- Patch SHA-256: `a1253543bc589f07edbc223e31ef0be0d691d1dae1d1d7595c00f55b0723c56b`
- Files changed:
  - `rust/lance/src/dataset.rs`
  - `rust/lance/src/dataset/tests/dataset_versioning.rs`

## Proposed API

```rust
pub async fn checkout_before(&self, cutoff: DateTime<Utc>) -> Result<Self>
```

The method lists the current branch's retained versions, keeps versions whose
native `DateTime<Utc>` timestamp is strictly before `cutoff`, selects the
greatest numeric version among them, and delegates to `checkout_version`. A
version removed by cleanup cannot be recovered, so the method reports a native
`InvalidInput` error that names the cutoff, the current branch, and the
retained-history limitation. Other listing, decoding, and checkout errors
propagate unchanged.

The receiver is not modified. A handle already checked out at an older version
still lists the branch's full retained history.

## Focused tests

Run name filter: `checkout_before`. Seven tests use `MockClock` to control
commit timestamps and multi-fragment datasets (`max_rows_per_file: 2`):

1. `checkout_before_selects_greatest_eligible_version`
2. `checkout_before_strict_nanosecond_boundary_and_equal_timestamps`
3. `checkout_before_no_match_reports_branch_and_retained_history`
4. `checkout_before_ignores_removed_history`
5. `checkout_before_old_handle_does_not_cap_history`
6. `checkout_before_limits_history_to_the_current_branch`
7. `checkout_before_propagates_manifest_read_failure`

Each positive case asserts both the selected version and the actual returned
rows. The tests cover nonmonotonic timestamps (max version, not max timestamp),
strict equality, equal timestamps, nanosecond precision, cleanup gaps, branch
isolation, old handles, and unchanged receivers.

## Runner

Use a disposable clone or worktree at the base commit. Do not point the runner
at the shared source checkout.

```bash
./run-native-checkout-before.sh /path/to/disposable/lance-checkout
```

The runner verifies the base commit, clean worktree, and `rustc 1.94.0`, applies
the patch, runs `cargo +1.94.0 fmt --all -- --check`, then runs:

```bash
cargo +1.94.0 test -j 8 -p lance --lib checkout_before -- --test-threads=4
```

It fails unless exactly seven focused tests run and pass. Build artifacts use
the caller's mbx-managed default target and cache; no overrides are applied.

## Validation evidence

To be recorded after execution on NieR.
