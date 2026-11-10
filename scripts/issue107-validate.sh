#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# One-step-at-a-time validation runner for issue 107 scanner introspection.
# This script belongs to the validation branch, not the product branch.

set -euo pipefail

if (( $# < 1 || $# > 3 )); then
    echo "usage: $0 <fmt|check|clippy|test|test-<integration-file>|c|cpp|typed|static>" >&2
    echo "       $0 test-filter <integration-file> <test-name>" >&2
    exit 2
fi

mode=$1
label=$mode
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
    test-filter)
        if (( $# != 3 )); then
            echo "test-filter requires an integration file and test name" >&2
            exit 2
        fi
        label="test-filter-$2-$3"
        cmd=(
            cargo +1.94.0 test --test "$2" -j 8 "$3"
            -- --test-threads=4
        )
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
esac

printf '%q ' "${cmd[@]}" > "$evidence/$label.command.tmp"
printf '\n' >> "$evidence/$label.command.tmp"
mv "$evidence/$label.command.tmp" "$evidence/$label.command"

git rev-parse HEAD > "$evidence/$label.head.tmp"
mv "$evidence/$label.head.tmp" "$evidence/$label.head"

date -u +%Y-%m-%dT%H:%M:%SZ > "$evidence/$label.started_at.tmp"
mv "$evidence/$label.started_at.tmp" "$evidence/$label.started_at"

set +e
"${cmd[@]}" > "$evidence/$label.log" 2>&1
status=$?
set -e

date -u +%Y-%m-%dT%H:%M:%SZ > "$evidence/$label.finished_at.tmp"
mv "$evidence/$label.finished_at.tmp" "$evidence/$label.finished_at"
printf '%s\n' "$status" > "$evidence/$label.status.tmp"
mv "$evidence/$label.status.tmp" "$evidence/$label.status"

exit "$status"
