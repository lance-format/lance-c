#!/usr/bin/env bash

set -u

if [[ $# -ne 4 ]]; then
    echo "usage: $0 <run-root> <product-commit> <product-tree> <validation-commit>" >&2
    exit 64
fi

RUN_ROOT=$1
PRODUCT_COMMIT=$2
PRODUCT_TREE=$3
EXPECTED_VALIDATION_COMMIT=$4
REPO_ROOT=$(git rev-parse --show-toplevel)
LOGS="$RUN_ROOT/logs"
META="$RUN_ROOT/meta"
STATUSES="$RUN_ROOT/statuses"

mkdir -p "$LOGS" "$META" "$STATUSES"

write_status() {
    local name=$1
    local value=$2
    printf '%s\n' "$value" > "$STATUSES/$name.status.tmp"
    mv "$STATUSES/$name.status.tmp" "$STATUSES/$name.status"
}

record_source_failure() {
    local message=$1
    printf '%s\n' "$message" > "$META/source-verification-error.txt"
    write_status 00-source FAIL
    write_status OVERALL FAIL
    echo "$message" >&2
    exit 2
}

cd "$REPO_ROOT"

ACTUAL_VALIDATION_COMMIT=$(git rev-parse HEAD)
ACTUAL_PRODUCT_TREE=$(git show -s --format=%T "$PRODUCT_COMMIT" 2>/dev/null || true)
REMOTE_URL=$(git remote get-url origin 2>/dev/null || true)
WORKTREE_STATUS=$(git status --porcelain)
PRODUCT_DIFF=$(git diff --name-only "$PRODUCT_COMMIT" HEAD 2>/dev/null || true)

{
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'hostname=%s\n' "$(hostname)"
    printf 'pwd=%s\n' "$REPO_ROOT"
    printf 'run_root=%s\n' "$RUN_ROOT"
    printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
    printf 'expected_product_tree=%s\n' "$PRODUCT_TREE"
    printf 'actual_product_tree=%s\n' "$ACTUAL_PRODUCT_TREE"
    printf 'validation_commit=%s\n' "$ACTUAL_VALIDATION_COMMIT"
    printf 'expected_validation_commit=%s\n' "$EXPECTED_VALIDATION_COMMIT"
    printf 'validation_tree=%s\n' "$(git rev-parse HEAD^{tree})"
    printf 'remote=%s\n' "$REMOTE_URL"
    printf 'product_diff_files=%s\n' "$(printf '%s' "$PRODUCT_DIFF" | tr '\n' ',')"
    printf 'git_status=%s\n' "$(printf '%s' "$WORKTREE_STATUS" | tr '\n' ',')"
    printf 'command_path=%s\n' "$PATH"
    printf 'cargo_shim=%s\n' "$(command -v cargo)"
    printf 'mbx=%s\n' "$(command -v mbx)"
    printf 'cargo_1_94=%s\n' "$(cargo +1.94.0 --version)"
    printf 'rustc_1_94=%s\n' "$(rustc +1.94.0 --version)"
    printf 'rustup_active=%s\n' "$(rustup show active-toolchain)"
    printf 'RUSTUP_TOOLCHAIN=%s\n' "${RUSTUP_TOOLCHAIN-<unset>}"
    printf 'CARGO_BUILD_JOBS=%s\n' "${CARGO_BUILD_JOBS-<unset>}"
    printf 'CARGO_TARGET_DIR=%s\n' "${CARGO_TARGET_DIR-<unset>}"
    printf 'CARGO_BUILD_TARGET_DIR=%s\n' "${CARGO_BUILD_TARGET_DIR-<unset>}"
    printf 'RUSTFLAGS=%s\n' "${RUSTFLAGS-<unset>}"
    printf 'CARGO_PROFILE_RELEASE_LTO=%s\n' "${CARGO_PROFILE_RELEASE_LTO-<unset>}"
    printf 'CARGO_PROFILE_DEV_LTO=%s\n' "${CARGO_PROFILE_DEV_LTO-<unset>}"
    printf 'target=%s\n' "$(readlink -f target 2>/dev/null || true)"
} > "$META/environment.txt"

if [[ "$REMOTE_URL" != "https://github.com/lance-format/lance-c" ]]; then
    record_source_failure "unexpected origin: $REMOTE_URL"
fi
if [[ "$ACTUAL_VALIDATION_COMMIT" != "$EXPECTED_VALIDATION_COMMIT" ]]; then
    record_source_failure "validation HEAD mismatch: $ACTUAL_VALIDATION_COMMIT != $EXPECTED_VALIDATION_COMMIT"
fi
if [[ "$ACTUAL_PRODUCT_TREE" != "$PRODUCT_TREE" ]]; then
    record_source_failure "product tree mismatch: $ACTUAL_PRODUCT_TREE != $PRODUCT_TREE"
fi
if [[ -n "$WORKTREE_STATUS" ]]; then
    record_source_failure "worktree is not clean: $WORKTREE_STATUS"
fi
if [[ "$PRODUCT_DIFF" != "validation/blob114-native-read.sh" ]]; then
    record_source_failure "validation branch contains unexpected changes: $PRODUCT_DIFF"
fi

write_status 00-source PASS

export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8

run_step() {
    local name=$1
    shift
    local log="$LOGS/$name.log"
    local command_text
    command_text=$(printf '%q ' "$@")
    printf '%s\n' "$command_text" > "$META/$name.command"
    printf 'started_utc=%s\ncommand=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$command_text" > "$META/$name.start"

    "$@" > "$log" 2>&1
    local rc=$?
    printf 'finished_utc=%s\nexit_code=%s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$rc" > "$META/$name.finish"
    if [[ $rc -eq 0 ]]; then
        write_status "$name" PASS
    else
        write_status "$name" "FAIL:$rc"
    fi
    return "$rc"
}

run_step 01-fmt cargo +1.94.0 fmt --all -- --check
run_step 02-check cargo +1.94.0 check --all-targets --locked
run_step 03-clippy cargo +1.94.0 clippy --all-targets --locked -- -D warnings
run_step 04-rust-tests cargo +1.94.0 test --locked -- --test-threads=4
run_step 05-compile-and-run-ignored \
    cargo +1.94.0 test --locked --test compile_and_run_test -- \
    --ignored --nocapture --test-threads=4
run_step 06-focused-blob-ffi \
    cargo +1.94.0 test --locked --test c_api_test -- blob_ --test-threads=4
run_step 07-focused-blob-prefetch \
    cargo +1.94.0 test --locked --lib -- \
    blob::tests::test_blob_file_set_buffer_size_controls_prefetch --exact

{
    printf '%s\n' '--- test totals ---'
    grep -hE '^test result:' "$LOGS"/04-rust-tests.log "$LOGS"/06-focused-blob-ffi.log \
        "$LOGS"/07-focused-blob-prefetch.log 2>/dev/null || true
    printf '%s\n' '--- normal-entry consumers ---'
    grep -hE 'Running C API tests|Running C\+\+ API tests|All C\+\+ tests passed|test_blob_native_read_slice|test_blob_take_by_addresses|test_blob_file_read_ranges' \
        "$LOGS"/05-compile-and-run-ignored.log 2>/dev/null || true
    printf '%s\n' '--- statuses ---'
    cat "$STATUSES"/0*.status 2>/dev/null || true
} > "$META/summary.txt"

FAILURES=0
for status_file in "$STATUSES"/0*.status; do
    if ! grep -q '^PASS$' "$status_file"; then
        FAILURES=$((FAILURES + 1))
    fi
done
if [[ $FAILURES -eq 0 ]]; then
    write_status OVERALL PASS
else
    write_status OVERALL "FAIL:$FAILURES"
fi

printf 'finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$META/environment.txt"
printf 'target_after=%s\n' "$(readlink -f target 2>/dev/null || true)" >> "$META/environment.txt"
(
    cd "$RUN_ROOT"
    sha256sum meta/* logs/* statuses/* | sort
) > "$RUN_ROOT/SHA256SUMS"

cat "$META/summary.txt"
if [[ $FAILURES -ne 0 ]]; then
    exit 1
fi
