# Native cooperative index cancellation candidate (#55/E4)

Validation-only source candidate against immutable Lance 14.0.2
`40f15ed1b71b66950961b175829fdf17a763f108`. It does not change the lance-c
pin, public C API, C headers, error numbers, or `ProgressCallbackGate`.

## Contents

- `CONTRACT.md` — accepted scope and acceptance boundaries.
- `native-cancellation.patch` — production-only patch (5 files).
- `e4_native_cancellation.rs` — focused native test copied into
  `rust/lance/tests/` by the runner.
- `run-nier.sh` — verifies the base, applies the patch, proves the exact
  post-image, checks formatting of changed files, and runs only the new target.

## What the candidate adds

- `IndexBuildProgress::check_cancelled` as a default-`Ok` async method, so
  existing observers compile unchanged.
- `CancellableIndexBuildProgress`, combining an observer and the workspace
  `tokio_util::sync::CancellationToken`.
- Public typed `IndexBuildCancelled` carried by `Error::External`, with
  `is_index_build_cancelled` for public downcast classification. Arbitrary
  observer errors keep their original identity.
- Startup checks in ordinary and multi-segment FM build entry points,
  a pre-return check for `execute_uncommitted`, and pre-publication checks in
  the ordinary route plus both trained and empty/untrained multi-segment FM
  routes.
- A real `build_pages` stage for BTREE (start/progress/complete plus
  per-page check) and cancellation checkpoints at vector partition planning,
  partition work, merge-writer creation, and per-output-partition merge.

## Tests

`cargo test -p lance --test e4_native_cancellation -- --test-threads=4`

1. `pre_cancelled_startup_returns_typed_cancellation_without_work`
2. `btree_page_build_cancellation_stops_long_stage`
3. `uncommitted_last_completion_window_returns_no_metadata`
4. `committed_last_completion_window_does_not_publish`
5. `observer_error_is_not_reclassified_as_cancellation`
6. `btree_success_control_reports_pages_and_publishes`
7. `vector_success_control_publishes_after_all_checkpoints`
8. `fm_trained_multi_segment_cancellation_before_publication` (plus success control)
9. `fm_untrained_multi_segment_cancellation_before_publication` (plus success control)

All use three fragments / 96 rows and deterministic callback triggers, not
sleeps. Cancellation cases reopen the dataset and assert unchanged version and
index set.

## Run on NieR

Use a task-owned clean checkout at the exact base (not a shared checkout with
other actors' edits):

```bash
export LANCE_SRC=/path/to/clean/lance-40f15ed
export EVIDENCE_DIR=/path/to/task/evidence
bash .validation/index-cancellation/run-nier.sh
```

The runner records `run.log`, `applied.patch`, `changed-production-files.txt`,
`status.txt`, artifact SHA-256 values, and an atomic `exit.txt`.

## Boundaries and remaining gaps

- Cancellation is best effort. A request observed at a checkpoint returns the
  typed error and publishes no index; a request arriving after the final check
  can lose to the commit. No linearizability is claimed.
- Completed but unreferenced segment files may remain for normal cleanup.
  Writer/shuffle temporary cleanup is preserved, but universal orphan deletion
  is not claimed.
- `spawn_cpu`/`tokio::spawn` work already in flight cannot be interrupted
  mid-compute; this candidate checks at task/partition boundaries and relies on
  dropping the build stream and writers on error.
- BTREE optimize/merge/remap callers still use the no-progress wrapper; vector
  shuffle/IVF/quantizer training internals and FM plugin training do not report
  additional checkpoints. Checks at the create/multi-segment level cover the
  publication route.
- No universal backend quiescence is claimed.
