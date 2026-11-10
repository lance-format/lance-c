#!/usr/bin/env bash

set -uo pipefail

: "${REPO_ROOT:?REPO_ROOT is required}"
: "${EVIDENCE_DIR:?EVIDENCE_DIR is required}"
: "${EXPECTED_SHA:?EXPECTED_SHA is required}"

LOGS="$EVIDENCE_DIR/logs"
STATUS="$EVIDENCE_DIR/status"
mkdir -p "$LOGS" "$STATUS"

export CARGO_BUILD_JOBS=8
export CARGO_TERM_COLOR=never
export RUSTUP_TOOLCHAIN=1.94.0

record_failure() {
    printf '%s\n' "$1" >"$EVIDENCE_DIR/FAILED.tmp"
    mv "$EVIDENCE_DIR/FAILED.tmp" "$EVIDENCE_DIR/FAILED"
}

actual_sha="$(git -C "$REPO_ROOT" rev-parse HEAD)"
if [[ "$actual_sha" != "$EXPECTED_SHA" ]]; then
    record_failure "expected $EXPECTED_SHA, found $actual_sha"
    exit 2
fi
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain=v1)" ]]; then
    record_failure "repository is not clean"
    exit 2
fi

{
    date -u
    hostname
    whoami
    printf 'repo=%s\n' "$REPO_ROOT"
    printf 'sha=%s\n' "$actual_sha"
    git -C "$REPO_ROOT" status --short --branch
    cargo +1.94.0 --version
    rustc +1.94.0 --version
    printf 'CARGO_BUILD_JOBS=%s\n' "$CARGO_BUILD_JOBS"
    env | grep -E '^(CARGO|RUST)' | sort
} >"$EVIDENCE_DIR/environment.txt" 2>&1

run_step() {
    local name="$1"
    shift
    local log="$LOGS/$name.log"
    "$@" >"$log" 2>&1
    local rc=$?
    printf '%s\n' "$rc" >"$STATUS/$name.status.tmp"
    mv "$STATUS/$name.status.tmp" "$STATUS/$name.status"
    if [[ $rc -ne 0 ]]; then
        record_failure "$name exited with $rc"
        exit "$rc"
    fi
}

cd "$REPO_ROOT" || exit 2

run_step fmt cargo +1.94.0 fmt --all -- --check
run_step check cargo +1.94.0 check --all-targets
run_step clippy cargo +1.94.0 clippy --all-targets -- -D warnings
run_step targeted-fragments cargo +1.94.0 test --test c_api_test fragment -- --test-threads=4
run_step rust-full cargo +1.94.0 test --all-targets -- --test-threads=4
run_step c-consumer cargo +1.94.0 test --test compile_and_run_test test_c_compilation_and_execution -- --ignored --exact --test-threads=1
run_step cpp-consumer cargo +1.94.0 test --test compile_and_run_test test_cpp_compilation_and_execution -- --ignored --exact --test-threads=1

printf '%s\n' "complete" >"$EVIDENCE_DIR/DONE.tmp"
mv "$EVIDENCE_DIR/DONE.tmp" "$EVIDENCE_DIR/DONE"
