#!/usr/bin/env bash
set -Eeuo pipefail

PRODUCT_COMMIT="${1:-}"
RUN_ROOT="${2:-}"
if [[ -z "$PRODUCT_COMMIT" || -z "$RUN_ROOT" ]]; then
    echo "usage: $0 <product-commit> <run-root>" >&2
    exit 2
fi
if [[ "$RUN_ROOT" != /home/xuanwo/Code/lance-c-validation/* ]]; then
    echo "run root must be under /home/xuanwo/Code/lance-c-validation" >&2
    exit 2
fi
if [[ -e "$RUN_ROOT" ]]; then
    echo "run root already exists: $RUN_ROOT" >&2
    exit 2
fi

mkdir -p "$RUN_ROOT/logs"
LOGS="$RUN_ROOT/logs"
STATUSES="$RUN_ROOT/statuses.tsv"
COMMANDS="$RUN_ROOT/commands.tsv"
META="$RUN_ROOT/meta.txt"
FINAL_STATUS="$RUN_ROOT/status"

export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

started_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
head_commit="$(git rev-parse HEAD)"
head_tree="$(git rev-parse 'HEAD^{tree}')"
product_tree="$(git rev-parse "${PRODUCT_COMMIT}^{tree}")"

write_status() {
    local result="$1"
    local exit_code="$2"
    local tmp="$FINAL_STATUS.tmp"
    {
        printf 'result=%s\n' "$result"
        printf 'exit_code=%s\n' "$exit_code"
        printf 'started_at=%s\n' "$started_at"
        printf 'finished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'head_commit=%s\n' "$head_commit"
        printf 'head_tree=%s\n' "$head_tree"
        printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
        printf 'product_tree=%s\n' "$product_tree"
    } >"$tmp"
    mv "$tmp" "$FINAL_STATUS"
}

on_exit() {
    local rc=$?
    if [[ $rc -eq 0 ]]; then
        write_status success 0
    else
        write_status failure "$rc"
    fi
}
trap on_exit EXIT

run_step() {
    local name="$1"
    shift
    local log="$LOGS/$name.log"
    local step_status="$LOGS/$name.status"
    local step_tmp="$step_status.tmp"
    local started finished rc
    started="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    {
        printf '%s\t' "$name"
        printf '%q ' "$@"
        printf '\n'
    } >>"$COMMANDS"
    printf '\n=== %s: %s ===\n' "$name" "$*"
    set +e
    "$@" 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    set -e
    finished="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    {
        printf 'name=%s\n' "$name"
        printf 'status=%s\n' "$([[ $rc -eq 0 ]] && echo success || echo failure)"
        printf 'exit_code=%s\n' "$rc"
        printf 'started_at=%s\n' "$started"
        printf 'finished_at=%s\n' "$finished"
        printf 'command='
        printf '%q ' "$@"
        printf '\n'
    } >"$step_tmp"
    mv "$step_tmp" "$step_status"
    {
        printf '%s\t%s\t%s\n' "$name" "$rc" "$finished"
    } >>"$STATUSES"
    if [[ $rc -ne 0 ]]; then
        echo "step failed: $name (exit $rc)" >&2
        exit "$rc"
    fi
}

assert_clean() {
    local label="$1"
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "$label source tree is dirty" >&2
        git status --short >&2
        exit 1
    fi
}

printf 'started_at=%s\n' "$started_at" >"$META"
{
    printf 'hostname=%s\n' "$(hostname)"
    printf 'user=%s\n' "$(id -un)"
    printf 'uname=%s\n' "$(uname -a)"
    printf 'pwd=%s\n' "$PWD"
    printf 'head_commit=%s\n' "$head_commit"
    printf 'head_tree=%s\n' "$head_tree"
    printf 'product_commit=%s\n' "$PRODUCT_COMMIT"
    printf 'product_tree=%s\n' "$product_tree"
    printf 'validation_diff_from_product:\n'
    git diff --name-status "$PRODUCT_COMMIT" -- . ':(exclude)validation'
    printf 'registered_checkout=%s\n' "${REGISTERED_CHECKOUT:-<unset>}"
    if [[ -n "${REGISTERED_CHECKOUT:-}" && -d "$REGISTERED_CHECKOUT" ]]; then
        printf 'registered_checkout_head=%s\n' \
            "$(git -C "$REGISTERED_CHECKOUT" rev-parse HEAD)"
        printf 'registered_checkout_status:\n'
        git -C "$REGISTERED_CHECKOUT" status --short
    fi
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'rust_test_threads=%s\n' "$RUST_TEST_THREADS"
    printf 'cargo_path=%s\n' "$(command -v cargo)"
    printf 'mbx_path=%s\n' "$(command -v mbx || true)"
    printf 'rustc=%s\n' "$(rustc --version)"
    printf 'cargo=%s\n' "$(cargo --version)"
    printf 'tmux_sessions_before:\n'
    tmux list-sessions 2>&1 || true
} >>"$META"

if [[ "$PWD" == "${REGISTERED_CHECKOUT:-}" ]]; then
    echo "validation must not run in the registered checkout" >&2
    exit 2
fi
if ! git merge-base --is-ancestor "$PRODUCT_COMMIT" HEAD; then
    echo "product commit is not an ancestor of HEAD" >&2
    exit 2
fi
unexpected_changes="$(
    git diff --name-only "$PRODUCT_COMMIT" -- . ':(exclude)validation'
)"
if [[ -n "$unexpected_changes" ]]; then
    printf 'validation branch changes product source:\n%s\n' "$unexpected_changes" >&2
    exit 2
fi
assert_clean "initial"

run_step rustc-version rustc --version
run_step cargo-version cargo --version

run_step fmt cargo fmt --all -- --check
run_step check cargo check --all-targets --locked
run_step clippy cargo clippy --all-targets --locked -- -D warnings
run_step focused-rtree cargo test --locked --test c_api_test rtree -- --nocapture
run_step full-rust cargo test --locked

run_step ignored-list \
    cargo test --locked --test compile_and_run_test -- --list --ignored
ignored_count="$(grep -c ': test$' "$LOGS/ignored-list.log" || true)"
printf 'ignored_compile_and_run_tests=%s\n' "$ignored_count" >>"$META"
if [[ "$ignored_count" -ne 5 ]]; then
    echo "expected 5 ignored compile_and_run_test consumers, found $ignored_count" >&2
    exit 1
fi

run_step ignored-consumers \
    cargo test --locked --test compile_and_run_test -- --ignored

assert_clean "final"
printf 'finished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$META"
echo "RTree scalar binding validation passed"
