#!/usr/bin/env bash
set -u

usage() {
    echo "usage: $0 <phase> <repo> <evidence-dir> <expected-sha>" >&2
    exit 64
}

phase=${1:-}
repo=${2:-}
evidence_dir=${3:-}
expected_sha=${4:-}
if [ -z "$phase" ] || [ -z "$repo" ] || [ -z "$evidence_dir" ] || [ -z "$expected_sha" ]; then
    usage
fi

mkdir -p "$evidence_dir"
log_file="$evidence_dir/$phase.log"
status_file="$evidence_dir/$phase.status"
meta_file="$evidence_dir/$phase.meta"
actual_sha=$(git -C "$repo" rev-parse HEAD 2>/dev/null || true)
status_tmp="$status_file.tmp.$$"

export CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-8}
export RUST_TEST_THREADS=${RUST_TEST_THREADS:-4}

publish_status() {
    printf 'exit_code=%s\n' "$1" > "$status_tmp"
    mv "$status_tmp" "$status_file"
}

{
    echo "phase=$phase"
    echo "repo=$repo"
    echo "expected_sha=$expected_sha"
    echo "actual_sha=$actual_sha"
    echo "head=$(git -C "$repo" log -1 --format='%H %s' 2>/dev/null || true)"
    echo "dirty_before=$(git -C "$repo" status --porcelain 2>/dev/null | wc -l)"
    echo "cargo=$(cargo --version 2>&1)"
    echo "rustc=$(rustc --version 2>&1)"
    if [[ "$phase" == core-* ]]; then
        echo "core_rustc=$(rustc +1.94.0 --version 2>&1)"
    fi
    echo "CARGO_BUILD_JOBS=$CARGO_BUILD_JOBS"
    echo "RUST_TEST_THREADS=$RUST_TEST_THREADS"
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$meta_file"

if [ -z "$actual_sha" ] || [ "$actual_sha" != "$expected_sha" ]; then
    echo "source_mismatch=expected $expected_sha, got ${actual_sha:-<none>}" >> "$meta_file"
    echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$meta_file"
    publish_status 97
    echo "source mismatch: expected $expected_sha, got ${actual_sha:-<none>}" >&2
    exit 97
fi

dirty=$(git -C "$repo" status --porcelain)
if [ -n "$dirty" ]; then
    {
        echo "dirty_source<<EOF"
        printf '%s\n' "$dirty"
        echo "EOF"
        echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >> "$meta_file"
    publish_status 98
    echo "source is dirty" >&2
    exit 98
fi

overall_rc=0
run_cmd() {
    {
        printf 'command='
        printf '%q ' "$@"
        printf '\n'
    } >> "$meta_file"
    "$@" >> "$log_file" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
        overall_rc=$rc
        {
            printf 'failed_command='
            printf '%q ' "$@"
            printf '\n'
            echo "failed_exit_code=$rc"
        } >> "$meta_file"
    fi
}

cd "$repo"
case "$phase" in
    fmt)
        run_cmd cargo fmt --all -- --check
        ;;
    check)
        run_cmd cargo check --all-targets
        ;;
    clippy)
        run_cmd cargo clippy --all-targets -- -D warnings
        ;;
    order)
        run_cmd cargo test --test c_api_test order_by
        run_cmd cargo test --test multivector_test multivector_rejects_order_by
        ;;
    full)
        run_cmd cargo test
        ;;
    native-c)
        run_cmd cargo test --test compile_and_run_test -- --ignored test_c_compilation_and_execution
        ;;
    native-cpp)
        run_cmd cargo test --test compile_and_run_test -- --ignored test_cpp_compilation_and_execution
        ;;
    core-check)
        run_cmd cargo +1.94.0 check --all-targets
        ;;
    core-clippy)
        run_cmd cargo +1.94.0 clippy --all-targets -- -D warnings
        ;;
    core-order)
        run_cmd cargo +1.94.0 test --test c_api_test order_by
        run_cmd cargo +1.94.0 test --test multivector_test multivector_rejects_order_by
        ;;
    core-full)
        run_cmd cargo +1.94.0 test
        ;;
    core-native-c)
        run_cmd cargo +1.94.0 test --test compile_and_run_test -- --ignored test_c_compilation_and_execution
        ;;
    core-native-cpp)
        run_cmd cargo +1.94.0 test --test compile_and_run_test -- --ignored test_cpp_compilation_and_execution
        ;;
    *)
        echo "unknown phase: $phase" >> "$meta_file"
        overall_rc=64
        ;;
esac

echo "finished_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$meta_file"
echo "exit_code=$overall_rc" >> "$meta_file"
publish_status "$overall_rc"
exit "$overall_rc"
