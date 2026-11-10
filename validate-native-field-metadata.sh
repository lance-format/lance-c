#!/usr/bin/env bash
set -euo pipefail

REPO_DIR=${1:?repository directory required}
ARTIFACT_DIR=${2:?artifact directory required}
LOG="$ARTIFACT_DIR/validation.log"
STATUS="$ARTIFACT_DIR/exit_status"
STATUS_TMP="$ARTIFACT_DIR/.exit_status.tmp"

mkdir -p "$ARTIFACT_DIR"
: > "$LOG"

finish() {
    local status=$?
    printf '%s\n' "$status" > "$STATUS_TMP"
    mv "$STATUS_TMP" "$STATUS"
    exit "$status"
}
trap finish EXIT

run() {
    printf '\n=== %s ===\n' "$*" >> "$LOG"
    set +e
    "$@" >> "$LOG" 2>&1
    local status=$?
    set -e
    printf 'EXIT %s\n' "$status" >> "$LOG"
    if [ "$status" -ne 0 ]; then
        return "$status"
    fi
}

cd "$REPO_DIR"
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

run git status --short --branch
run git rev-parse HEAD
run git diff --check
run cargo +1.94.0 fmt --check
run cargo +1.94.0 check --all-targets
run cargo +1.94.0 clippy --all-targets -- -D warnings
run cargo +1.94.0 test --test c_api_test test_dataset_field_metadata_batch_matches_native_builder -- --exact
run cargo +1.94.0 test --test c_api_test test_dataset_field_metadata_semantics_errors_and_snapshots -- --exact
run cargo +1.94.0 test --test c_api_test test_dataset_snapshot_inspection_matches_native -- --exact
run cargo +1.94.0 test
run cargo +1.94.0 test --test compile_and_run_test -- --ignored
