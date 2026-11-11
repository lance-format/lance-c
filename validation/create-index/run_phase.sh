#!/usr/bin/env bash
set -uo pipefail

phase=${1:?usage: run_phase.sh <phase> <repo> <out>}
repo=${2:?usage: run_phase.sh <phase> <repo> <out>}
out=${3:?usage: run_phase.sh <phase> <repo> <out>}

log_dir="$out/logs"
mkdir -p "$log_dir"
export PATH="$HOME/.local/bin:$PATH"
export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

cd "$repo"

run() {
    local name=$1
    shift
    local log="$log_dir/$phase.$name.log"
    {
        printf 'COMMAND:'
        printf ' %q' "$@"
        printf '\n\n'
    } >"$log"
    printf 'phase=%s gate=%s log=%s\n' "$phase" "$name" "$log"
    if "$@" >>"$log" 2>&1; then
        printf 'phase=%s gate=%s status=ok log=%s\n' "$phase" "$name" "$log"
        return 0
    else
        local rc=$?
        printf 'phase=%s gate=%s status=failed rc=%s log=%s\n' "$phase" "$name" "$rc" "$log"
        return "$rc"
    fi
}

case "$phase" in
fmt)
    run fmt cargo +1.94.0 fmt --all -- --check
    ;;
check)
    run check cargo +1.94.0 check --all-targets --locked
    ;;
clippy)
    run clippy cargo +1.94.0 clippy --all-targets --locked -- -D warnings
    ;;
focused)
    failed=0
    run metadata-clone cargo +1.94.0 test --locked --lib \
        index_segment::tests::metadata_clone_preserves_in_memory_optionals_without_protobuf \
        -- --exact || failed=1
    run metadata-inspection cargo +1.94.0 test --locked --test c_api_test \
        test_index_metadata_complete_inspection_clone_and_native_oracle -- --exact || failed=1
    run constructor cargo +1.94.0 test --locked --test c_api_test \
        test_create_index_operation_constructor_validation_and_ownership -- --exact || failed=1
    run commit-replace-history cargo +1.94.0 test --locked --test c_api_test \
        test_create_index_commit_replace_conflict_history_and_query -- --exact || failed=1
    run native-validation cargo +1.94.0 test --locked --test c_api_test \
        test_create_index_native_validation_stays_in_core -- --exact || failed=1
    exit "$failed"
    ;;
full)
    run full cargo +1.94.0 test --all-targets --locked -- --test-threads=4
    ;;
ignored)
    run native-consumers cargo +1.94.0 test --test compile_and_run_test \
        --locked -- --ignored --test-threads=1
    ;;
*)
    printf 'unknown phase: %s\n' "$phase" >&2
    exit 2
    ;;
esac
