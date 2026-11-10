#!/usr/bin/env bash
set -euo pipefail

source_dir=${1:?source directory}
expected_commit=${2:?expected product commit}
evidence_dir=${3:?evidence directory}

mkdir -p "$evidence_dir"
log="$evidence_dir/validation.log"
status_file="$evidence_dir/exit-status"
rm -f "$status_file"
: >"$log"

finish() {
    local rc=$?
    local tmp="$status_file.tmp"
    printf '%s\n' "$rc" >"$tmp"
    mv "$tmp" "$status_file"
}
trap finish EXIT

run_step() {
    local name=$1
    shift
    printf '\n=== %s ===\n' "$name" | tee -a "$log"
    printf 'command:' | tee -a "$log"
    printf ' %q' "$@" | tee -a "$log"
    printf '\n' | tee -a "$log"

    set +e
    "$@" 2>&1 | tee -a "$log"
    local rc=${PIPESTATUS[0]}
    set -e

    printf 'exit: %s\n' "$rc" | tee -a "$log"
    if [ "$rc" -ne 0 ]; then
        exit "$rc"
    fi
}

cd "$source_dir"

actual_commit=$(git rev-parse HEAD)
if [ "$actual_commit" != "$expected_commit" ]; then
    printf 'commit mismatch: expected %s, got %s\n' "$expected_commit" "$actual_commit" |
        tee -a "$log"
    exit 2
fi
if [ -n "$(git status --short)" ]; then
    printf 'source tree is not clean before validation\n' | tee -a "$log"
    git status --short | tee -a "$log"
    exit 3
fi

export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

{
    printf 'started_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'host=%s\n' "$(hostname)"
    printf 'source_dir=%s\n' "$source_dir"
    printf 'commit=%s\n' "$actual_commit"
    printf 'branch=%s\n' "$(git branch --show-current)"
    printf 'cargo=%s\n' "$(cargo +1.94.0 --version)"
    printf 'rustc=%s\n' "$(rustc +1.94.0 --version)"
    printf 'CARGO_BUILD_JOBS=%s\n' "$CARGO_BUILD_JOBS"
    printf 'RUST_TEST_THREADS=%s\n' "$RUST_TEST_THREADS"
    printf 'source_status=%s\n' "$(git status --short | tr '\n' ';')"
} | tee -a "$log"

run_step cargo_fmt_check cargo +1.94.0 fmt --all -- --check
run_step cargo_check_all_targets cargo +1.94.0 check --all-targets
run_step focused_metadata_tests \
    cargo +1.94.0 test --test c_api_test test_dataset_metadata -- --test-threads=4
run_step cargo_clippy \
    cargo +1.94.0 clippy --all-targets -- -D warnings
run_step full_rust_tests cargo +1.94.0 test
run_step c_consumer \
    cargo +1.94.0 test --test compile_and_run_test \
    test_c_compilation_and_execution -- --ignored --exact --test-threads=4
run_step cpp_consumer \
    cargo +1.94.0 test --test compile_and_run_test \
    test_cpp_compilation_and_execution -- --ignored --exact --test-threads=4

if [ -n "$(git status --short)" ]; then
    printf 'source tree is not clean after validation\n' | tee -a "$log"
    git status --short | tee -a "$log"
    exit 4
fi

printf '\nfinished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" | tee -a "$log"
printf 'validation passed\n' | tee -a "$log"
