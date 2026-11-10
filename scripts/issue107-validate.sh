#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# One-step-at-a-time validation runner for issue 107 scanner introspection.
# This script belongs to the validation branch, not the product branch.

set -euo pipefail

if (( $# != 1 )); then
    echo "usage: $0 <fmt|check|clippy|test|test-<integration-file>|c|cpp|typed|static>" >&2
    exit 2
fi

mode=$1
evidence=${EVIDENCE_DIR:-$(pwd)/evidence}
mkdir -p "$evidence"

case "$mode" in
    fmt)
        cmd=(cargo +1.94.0 fmt --all -- --check)
        ;;
    check)
        cmd=(cargo +1.94.0 check --all-targets -j 8)
        ;;
    clippy)
        cmd=(cargo +1.94.0 clippy --all-targets -j 8 -- -D warnings)
        ;;
    test)
        cmd=(cargo +1.94.0 test -j 8 -- --test-threads=4)
        ;;
    c)
        cmd=(
            cargo +1.94.0 test --test compile_and_run_test
            test_c_compilation_and_execution
            -- --ignored --nocapture
        )
        ;;
    cpp)
        cmd=(
            cargo +1.94.0 test --test compile_and_run_test
            test_cpp_compilation_and_execution
            -- --ignored --nocapture
        )
        ;;
    typed)
        cmd=(
            cargo +1.94.0 test --test compile_and_run_test
            test_typed_fts_native_compilation_and_execution
            -- --ignored --nocapture
        )
        ;;
    static)
        cmd=(
            cargo +1.94.0 test --test compile_and_run_test
            test_static_oss_transport
            -- --ignored --nocapture
        )
        ;;
    test-*)
        cmd=(
            cargo +1.94.0 test --test "${mode#test-}" -j 8
            -- --test-threads=4
        )
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
esac

printf '%q ' "${cmd[@]}" > "$evidence/$mode.command.tmp"
printf '\n' >> "$evidence/$mode.command.tmp"
mv "$evidence/$mode.command.tmp" "$evidence/$mode.command"

git rev-parse HEAD > "$evidence/$mode.head.tmp"
mv "$evidence/$mode.head.tmp" "$evidence/$mode.head"

date -u +%Y-%m-%dT%H:%M:%SZ > "$evidence/$mode.started_at.tmp"
mv "$evidence/$mode.started_at.tmp" "$evidence/$mode.started_at"

set +e
"${cmd[@]}" > "$evidence/$mode.log" 2>&1
status=$?
set -e

date -u +%Y-%m-%dT%H:%M:%SZ > "$evidence/$mode.finished_at.tmp"
mv "$evidence/$mode.finished_at.tmp" "$evidence/$mode.finished_at"
printf '%s\n' "$status" > "$evidence/$mode.status.tmp"
mv "$evidence/$mode.status.tmp" "$evidence/$mode.status"

exit "$status"
