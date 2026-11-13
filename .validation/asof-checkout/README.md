# Native strict-before checkout candidate

This directory contains a validation-only candidate for lance-c #108. It adds
one native API and its focused tests on top of immutable Lance 14.0.2. It does
not change the C ABI, C implementation, dependency pins, error numbers, MSRV,
or Python code.

## Base

- Repository: `lance-format/lance`
- Commit: `40f15ed1b71b66950961b175829fdf17a763f108` (Lance 14.0.2)
- Patch: `native-checkout-before.patch`
- Patch SHA-256: `aeacfdd647e992aaf72ab7a5064e9521f0439863faed5114de657c9f5316b854`
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
isolation, old handles, and unchanged receivers. The final test corrupts a
retained manifest while keeping it listed, so the read failure cannot be hidden
by a shorter listing.

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

Executed on NieR on 2026-11-13 against a disposable clone at base
`40f15ed1b71b66950961b175829fdf17a763f108` with Rust `1.94.0`
(`4a4ef493e`) and Cargo `1.94.0`. The runner checked the base and clean tree,
applied the patch, and ran the formatting check and focused tests:

```bash
cargo +1.94.0 fmt --all -- --check
cargo +1.94.0 test -j 8 -p lance --lib checkout_before -- --test-threads=4
```

Results: formatting exited 0; exactly 7 tests ran and passed, with 6301 tests
filtered out (`test result: ok. 7 passed; 0 failed; 0 ignored`); runner exit 0.
The test log is retained at `logs/run-4.log` in the NieR validation workspace
and at `/tmp/asof-checkout-before.S8uaHE.log` from that run. Builds used the
mbx default target and cache without overrides.

## Boundaries

- This is a validation-only native candidate; it does not add the C binding,
  change dependency pins, error numbers, or MSRV, or change Python code.
- The API accepts a native `DateTime<Utc>` and compares native timestamps at
  nanosecond precision. Python's local/naive/microsecond conversions remain a
  separate binding limit; this patch does not claim those cases are equivalent.
- Only the current branch's attached, retained history is searched. Versions
  removed by cleanup are unrecoverable and appear only as a shortened history
  or the documented `InvalidInput` error.
- The focused tests cover the seven cases listed above; full C/C++ suites were
  not run because no C surface changed.
