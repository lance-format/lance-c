#!/usr/bin/env bash
set -euo pipefail

RUN_DIR=${1:?usage: issue108-native-branch-lifecycle.sh <run-dir>}
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
mkdir -p "$RUN_DIR"

export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export RUSTUP_TOOLCHAIN=1.94.0

status_file="$RUN_DIR/exit-status"
status_tmp="$RUN_DIR/.exit-status.tmp"
printf 'running\n' > "$status_tmp"
trap 'rc=$?; printf "%s\n" "$rc" > "$status_tmp"; mv "$status_tmp" "$status_file"' EXIT

cd "$REPO_ROOT"

{
    printf 'hostname=%s\n' "$(hostname)"
    printf 'pwd=%s\n' "$PWD"
    printf 'head=%s\n' "$(git rev-parse HEAD)"
    printf 'tree=%s\n' "$(git rev-parse 'HEAD^{tree}')"
    printf 'cargo=%s\n' "$(cargo +1.94.0 --version)"
    printf 'rustc=%s\n' "$(rustc +1.94.0 --version)"
    printf 'cc=%s\n' "$(cc --version | head -1)"
    printf 'cxx=%s\n' "$(c++ --version | head -1)"
    printf 'CARGO_BUILD_JOBS=%s\n' "$CARGO_BUILD_JOBS"
    printf 'RUST_TEST_THREADS=%s\n' "$RUST_TEST_THREADS"
    printf 'RUSTUP_TOOLCHAIN=%s\n' "$RUSTUP_TOOLCHAIN"
    git status --porcelain=v1
} > "$RUN_DIR/identity.log"

run_step() {
    local name=$1
    shift
    local log="$RUN_DIR/$name.log"
    local step_status="$RUN_DIR/$name.status"
    printf '$ %s\n' "$*" | tee "$log"
    set +e
    "$@" 2>&1 | tee -a "$log"
    local rc=${PIPESTATUS[0]}
    set -e
    printf '%s\n' "$rc" > "$step_status"
    printf '%s rc=%s log=%s\n' "$name" "$rc" "$log"
    if ((rc != 0)); then
        exit "$rc"
    fi
}

run_step fmt cargo +1.94.0 fmt --check
run_step check cargo +1.94.0 check --all-targets
run_step clippy cargo +1.94.0 clippy --all-targets -- -D warnings
run_step focused_branch_tests cargo +1.94.0 test --test c_api_test test_branch_
run_step full_test cargo +1.94.0 test
run_step native_consumers cargo +1.94.0 test --test compile_and_run_test -- --ignored

test -z "$(git status --porcelain=v1)"
printf 'clean\n' > "$RUN_DIR/tree-clean.status"
