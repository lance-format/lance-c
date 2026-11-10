#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
set -uo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <phase> <source-dir> <run-dir>" >&2
    exit 2
fi

phase=$1
source_dir=$2
run_dir=$3
toolchain=1.94.0

case "$phase" in
    fmt) command=(cargo "+$toolchain" fmt --check) ;;
    check) command=(cargo "+$toolchain" check --all-targets --locked) ;;
    clippy) command=(cargo "+$toolchain" clippy --all-targets --locked -- -D warnings) ;;
    focused-materialization) command=(cargo "+$toolchain" test --locked --test scanner_materialization_fast_search_test materialization_ -- --nocapture) ;;
    focused-fast-search) command=(cargo "+$toolchain" test --locked --test scanner_materialization_fast_search_test fast_search_ -- --nocapture) ;;
    focused-unit) command=(cargo "+$toolchain" test --locked --lib scanner_materialization_and_fast_search_setters_preserve_state_on_errors -- --nocapture) ;;
    all-rust) command=(cargo "+$toolchain" test --locked) ;;
    native) command=(cargo "+$toolchain" test --locked --test compile_and_run_test -- --ignored --test-threads=1 --nocapture) ;;
    c-consumer) command=(cargo "+$toolchain" test --locked --test compile_and_run_test -- --ignored --nocapture test_c_compilation_and_execution) ;;
    cpp-consumer) command=(cargo "+$toolchain" test --locked --test compile_and_run_test -- --ignored --nocapture test_cpp_compilation_and_execution) ;;
    *)
        echo "unknown phase: $phase" >&2
        exit 2
        ;;
esac

mkdir -p "$run_dir"
if [ -e "$run_dir/exit_status" ]; then
    echo "refusing to overwrite completed run: $run_dir" >&2
    exit 2
fi
if [ -e "$run_dir/log.txt" ]; then
    echo "refusing to overwrite existing log: $run_dir" >&2
    exit 2
fi

commit=$(git -C "$source_dir" rev-parse HEAD)
dirty=$(git -C "$source_dir" status --porcelain=v1)
meta_tmp="$run_dir/.meta.tmp"
{
    printf 'phase=%s\n' "$phase"
    printf 'host=%s\n' "$(hostname -f 2>/dev/null || hostname)"
    printf 'started_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'source_dir=%s\n' "$source_dir"
    printf 'source_commit=%s\n' "$commit"
    printf 'source_branch=%s\n' "$(git -C "$source_dir" symbolic-ref --short -q HEAD || echo DETACHED)"
    printf 'source_dirty=%s\n' "$([ -n "$dirty" ] && echo true || echo false)"
    printf 'toolchain=%s\n' "$toolchain"
    printf 'cargo_build_jobs=8\n'
    printf 'rust_test_threads=4\n'
    printf 'command='
    printf '%q ' "${command[@]}"
    printf '\n'
} > "$meta_tmp"
mv "$meta_tmp" "$run_dir/meta.txt"
command_tmp="$run_dir/.command.tmp"
printf '%q ' "${command[@]}" > "$command_tmp"
printf '\n' >> "$command_tmp"
mv "$command_tmp" "$run_dir/command.txt"

cd "$source_dir" || exit 2
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
"${command[@]}" > "$run_dir/log.txt" 2>&1
status=$?
meta_tmp="$run_dir/.meta.tmp"
cp "$run_dir/meta.txt" "$meta_tmp"
printf 'finished_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$meta_tmp"
mv "$meta_tmp" "$run_dir/meta.txt"
printf '%s\n' "$status" > "$run_dir/.exit_status.tmp"
mv "$run_dir/.exit_status.tmp" "$run_dir/exit_status"
exit "$status"
