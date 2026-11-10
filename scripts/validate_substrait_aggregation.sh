#!/usr/bin/env bash
set -euo pipefail

PRODUCT_DIR=${1:?product directory}
EVIDENCE_DIR=${2:?evidence directory}
EXPECTED_HEAD=${3:?expected product head}
EXPECTED_TREE=${4:?expected product tree}
REGISTERED_REPO=${5:-/home/xuanwo/Code/lance-format/lance-c}
REGISTERED_HEAD=${6:-a51ab47a9c814a746cb1714c99fad93dd3a188b8}

mkdir -p "$EVIDENCE_DIR"

overall_exit="$EVIDENCE_DIR/status"
record_overall_exit() {
    local code=$?
    printf '%s\n' "$code" >"$overall_exit.tmp"
    mv "$overall_exit.tmp" "$overall_exit"
    {
        printf 'exit=%s\n' "$code"
        printf 'finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$EVIDENCE_DIR/status.meta.txt.tmp"
    mv "$EVIDENCE_DIR/status.meta.txt.tmp" "$EVIDENCE_DIR/status.meta.txt"
}
trap record_overall_exit EXIT

write_preflight() {
    local tmp="$EVIDENCE_DIR/preflight.txt.tmp"
    {
        printf 'host=%s\n' "$(hostname)"
        printf 'product_dir=%s\n' "$PRODUCT_DIR"
        printf 'expected_head=%s\n' "$EXPECTED_HEAD"
        printf 'expected_tree=%s\n' "$EXPECTED_TREE"
        printf 'registered_repo=%s\n' "$REGISTERED_REPO"
        printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'rustc=%s\n' "$(rustc +1.94.0 --version)"
        printf 'cargo=%s\n' "$(cargo +1.94.0 --version)"
        printf 'RUSTUP_TOOLCHAIN=%s\n' "${RUSTUP_TOOLCHAIN-}"
        printf 'CARGO_BUILD_JOBS=%s\n' "${CARGO_BUILD_JOBS-}"
        printf 'RUST_TEST_THREADS=%s\n' "${RUST_TEST_THREADS-}"
        printf 'CARGO_TARGET_DIR=%s\n' "${CARGO_TARGET_DIR-}"
        printf 'CARGO_BUILD_BUILD_DIR=%s\n' "${CARGO_BUILD_BUILD_DIR-}"
        printf 'TMUX=%s\n' "${TMUX-}"
        printf 'TMUX_PANE=%s\n' "${TMUX_PANE-}"
    } >"$tmp"
    mv "$tmp" "$EVIDENCE_DIR/preflight.txt"
}

verify_source() {
    local actual_head actual_tree
    actual_head=$(git -C "$PRODUCT_DIR" rev-parse HEAD)
    actual_tree=$(git -C "$PRODUCT_DIR" rev-parse HEAD^{tree})
    if [[ "$actual_head" != "$EXPECTED_HEAD" ]]; then
        printf 'product HEAD mismatch: expected %s, got %s\n' \
            "$EXPECTED_HEAD" "$actual_head" >&2
        return 2
    fi
    if [[ "$actual_tree" != "$EXPECTED_TREE" ]]; then
        printf 'product tree mismatch: expected %s, got %s\n' \
            "$EXPECTED_TREE" "$actual_tree" >&2
        return 2
    fi
    if [[ -n "$(git -C "$PRODUCT_DIR" status --porcelain=v1)" ]]; then
        printf 'product worktree is not clean\n' >&2
        return 2
    fi
}

record_registered_repo() {
    local suffix=$1
    if [[ -d "$REGISTERED_REPO/.git" ]]; then
        {
            printf 'head=%s\n' "$(git -C "$REGISTERED_REPO" rev-parse HEAD)"
            printf 'tree=%s\n' "$(git -C "$REGISTERED_REPO" rev-parse HEAD^{tree})"
            printf 'branch=%s\n' "$(git -C "$REGISTERED_REPO" branch --show-current)"
            git -C "$REGISTERED_REPO" status --porcelain=v1
        } >"$EVIDENCE_DIR/registered-repo-$suffix.txt"
        [[ "$(git -C "$REGISTERED_REPO" rev-parse HEAD)" == "$REGISTERED_HEAD" ]]
    else
        printf 'missing registered repo: %s\n' "$REGISTERED_REPO" \
            >"$EVIDENCE_DIR/registered-repo-$suffix.txt"
        return 1
    fi
}

write_preflight
verify_source
record_registered_repo before
printf 'source_clean=yes\n' >>"$EVIDENCE_DIR/preflight.txt"

cd "$PRODUCT_DIR"
export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export CARGO_TERM_COLOR=never
export RUST_BACKTRACE=1

run_phase() {
    local name=$1
    shift
    local log="$EVIDENCE_DIR/$name.log"
    local command_tmp="$EVIDENCE_DIR/$name.command.txt.tmp"
    local meta_tmp="$EVIDENCE_DIR/$name.meta.txt.tmp"
    local exit_tmp="$EVIDENCE_DIR/$name.exit.tmp"
    local start end code

    {
        printf 'cwd=%q' "$PWD"
        printf ' %q' "$@"
        printf '\n'
    } >"$command_tmp"
    mv "$command_tmp" "${command_tmp%.tmp}"

    start=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    {
        printf 'phase=%s\n' "$name"
        printf 'started_utc=%s\n' "$start"
        printf 'host=%s\n' "$(hostname)"
        printf 'product_head=%s\n' "$(git rev-parse HEAD)"
        printf 'product_tree=%s\n' "$(git rev-parse HEAD^{tree})"
        printf 'TMUX_PANE=%s\n' "${TMUX_PANE-}"
    } >"$meta_tmp"
    mv "$meta_tmp" "${meta_tmp%.tmp}"

    set +e
    "$@" >"$log.tmp" 2>&1
    code=$?
    set -e
    mv "$log.tmp" "$log"

    end=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    {
        printf 'finished_utc=%s\n' "$end"
        printf 'exit=%s\n' "$code"
    } >>"$EVIDENCE_DIR/$name.meta.txt"
    printf '%s\n' "$code" >"$exit_tmp"
    mv "$exit_tmp" "$EVIDENCE_DIR/$name.exit"
    printf '[%s] exit %s\n' "$name" "$code"
    return "$code"
}

run_phase fmt cargo +1.94.0 fmt --all -- --check
run_phase check cargo +1.94.0 check --all-targets --locked
run_phase clippy cargo +1.94.0 clippy --all-targets --locked -- -D warnings
run_phase focused_c_api cargo +1.94.0 test --locked --test c_api_test \
    test_scanner_substrait_aggregate -- --nocapture --test-threads=4
run_phase focused_batch_nearest cargo +1.94.0 test --locked \
    --test batch_nearest_test \
    batch_nearest_and_flat_nearest_compose_with_substrait_aggregate \
    -- --nocapture --test-threads=4
run_phase focused_multivector cargo +1.94.0 test --locked \
    --test multivector_test multivector_aggregate \
    -- --nocapture --test-threads=4
run_phase focused_prepared_fts cargo +1.94.0 test --locked \
    --test row_addr_prefilter_test \
    test_row_addr_allowlist_prepared_fts_aggregate_composition \
    -- --nocapture --test-threads=4
run_phase full_rust cargo +1.94.0 test --locked -- --test-threads=4
run_phase native_consumers cargo +1.94.0 test --locked \
    --test compile_and_run_test -- --ignored --test-threads=1 --nocapture

verify_source
record_registered_repo after

summary_tmp="$EVIDENCE_DIR/summary.txt.tmp"
{
    printf 'product_head=%s\n' "$(git rev-parse HEAD)"
    printf 'product_tree=%s\n' "$(git rev-parse HEAD^{tree})"
    printf 'product_status=%s\n' "$(git status --porcelain=v1 --branch | head -1)"
    printf 'rustc=%s\n' "$(rustc +1.94.0 --version)"
    printf 'cargo=%s\n' "$(cargo +1.94.0 --version)"
    for exit_file in "$EVIDENCE_DIR"/*.exit; do
        printf '%s=%s\n' "$(basename "$exit_file" .exit)" "$(cat "$exit_file")"
    done
    printf '\nRust summaries:\n'
    grep -H 'test result:' "$EVIDENCE_DIR"/*.log || true
    printf '\nNative probe markers:\n'
    grep -H -E 'All C tests passed|All C\+\+ tests passed|All typed FTS|PASS:' \
        "$EVIDENCE_DIR"/native_consumers.log || true
} >"$summary_tmp"
mv "$summary_tmp" "$EVIDENCE_DIR/summary.txt"

sha_tmp="$EVIDENCE_DIR/SHA256SUMS.tmp"
(
    cd "$EVIDENCE_DIR"
    sha256sum ./* | grep -v './SHA256SUMS' >"$sha_tmp.partial"
)
mv "$sha_tmp.partial" "$EVIDENCE_DIR/SHA256SUMS"

printf 'aggregate=green\n' >"$EVIDENCE_DIR/aggregate_status.txt.tmp"
mv "$EVIDENCE_DIR/aggregate_status.txt.tmp" \
    "$EVIDENCE_DIR/aggregate_status.txt"
