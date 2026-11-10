#!/usr/bin/env bash
# Validation-only runner for the lance-c issue 118 change. This script is not
# part of the product commit; it records each check and writes status files
# atomically without deleting earlier runs.
set -euo pipefail

: "${EXPECTED_COMMIT:?EXPECTED_COMMIT must name the exact commit under test}"
: "${REPO:?REPO must name the clean checkout under test}"

RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
LOG_ROOT="${LOG_ROOT:-/home/xuanwo/exec/logs/lance-c-118-$EXPECTED_COMMIT}"
LOG_DIR="$LOG_ROOT/$RUN_ID"
mkdir -p "$LOG_DIR"

export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

write_final() {
    local state="$1"
    local failed_step="$2"
    local exit_code="$3"
    local tmp="$LOG_DIR/final-status.json.tmp"
    printf '{"run_id":"%s","commit":"%s","state":"%s","failed_step":"%s","exit":%d,"finished_at":"%s"}\n' \
        "$RUN_ID" "$EXPECTED_COMMIT" "$state" "$failed_step" "$exit_code" \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$tmp"
    mv "$tmp" "$LOG_DIR/final-status.json"
}

write_step_status() {
    local step="$1"
    local exit_code="$2"
    local tmp="$LOG_DIR/$step.status.json.tmp"
    printf '{"step":"%s","exit":%d,"finished_at":"%s"}\n' \
        "$step" "$exit_code" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$tmp"
    mv "$tmp" "$LOG_DIR/$step.status.json"
}

run_step() {
    local step="$1"
    shift
    local log="$LOG_DIR/$step.log"
    printf '[%s] START %s: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$step" "$*" \
        | tee -a "$LOG_DIR/driver.log"
    set +e
    "$@" >"$log" 2>&1
    local exit_code=$?
    set -e
    write_step_status "$step" "$exit_code"
    printf '[%s] END %s: exit=%d\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$step" "$exit_code" \
        | tee -a "$LOG_DIR/driver.log"
    if [ "$exit_code" -ne 0 ]; then
        write_final "failed" "$step" "$exit_code"
        exit "$exit_code"
    fi
}

cd "$REPO"

actual_commit="$(git rev-parse HEAD)"
if [ "$actual_commit" != "$EXPECTED_COMMIT" ]; then
    printf 'expected commit %s, found %s\n' "$EXPECTED_COMMIT" "$actual_commit" >&2
    write_final "failed" "commit_identity" 1
    exit 1
fi
if [ -n "$(git status --porcelain=v1)" ]; then
    printf 'source tree is not clean\n' >&2
    write_final "failed" "clean_source" 1
    exit 1
fi

write_final "running" "" 0
printf '[%s] commit=%s run_id=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "$actual_commit" "$RUN_ID" | tee -a "$LOG_DIR/driver.log"

run_step "fmt" cargo +1.94.0 fmt --all -- --check
run_step "check" cargo +1.94.0 check --all-targets
run_step "clippy" cargo +1.94.0 clippy --all-targets -- -D warnings
run_step "rust_tests" cargo +1.94.0 test
run_step "native_consumers" \
    cargo +1.94.0 test --test compile_and_run_test -- --ignored

write_final "passed" "" 0
printf '[%s] ALL STEPS PASSED\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    | tee -a "$LOG_DIR/driver.log"
