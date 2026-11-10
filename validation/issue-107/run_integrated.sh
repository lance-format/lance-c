#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
set -uo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <run-directory>" >&2
    exit 2
fi

run_dir=$1
source_dir=$(git rev-parse --show-toplevel)
mkdir -p "$run_dir"
if [ -e "$run_dir/status" ]; then
    echo "refusing to overwrite an existing result" >&2
    exit 2
fi

status=0
for phase in fmt check clippy all-rust c-consumer cpp-consumer; do
    echo "Running $phase"
    bash "$source_dir/validation/issue-107/run_phase.sh" "$phase" "$source_dir" "$run_dir/$phase"
    status=$?
    echo "Finished $phase with status $status"
    if [ "$status" -ne 0 ]; then
        break
    fi
done
printf '%s\n' "$status" > "$run_dir/.status.tmp"
mv "$run_dir/.status.tmp" "$run_dir/status"
exit "$status"
