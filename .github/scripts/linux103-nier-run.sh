#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Run one x86_64 Linux candidate row on NieR. This is temporary issue-103
# validation support; it is not product code.

set -euo pipefail
export LC_ALL=C

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <host|debian> <base-dir>" >&2
    exit 2
fi

mode=$1
base=$2
case "$mode" in
    host | debian) ;;
    *)
        echo "unsupported mode: $mode" >&2
        exit 2
        ;;
esac

branch_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
source_root="$base/source"
work_root="$base/work-$mode"

mkdir -p "$base"
exec >"$base/driver-$mode.log" 2>&1

status_file="$base/status-$mode.txt"
on_exit() {
    local rc=$?
    printf 'exit=%d at %s\n' \
        "$rc" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >"$status_file"
    exit "$rc"
}
trap on_exit EXIT

printf 'utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf 'mode=%s\n' "$mode"
printf 'branch_root=%s\n' "$branch_root"
printf 'source_root=%s\n' "$source_root"
printf 'work_root=%s\n' "$work_root"
printf 'host=%s\n' "$(uname -a)"
printf 'branch_head=%s\n' "$(git -C "$branch_root" rev-parse HEAD)"

if [ ! -d "$source_root/.git" ]; then
    git clone --no-checkout \
        https://github.com/lance-format/lance-c "$source_root"
fi
git -C "$source_root" fetch origin
git -C "$source_root" checkout --detach \
    009d33f85cb7ac087d0c25add1161dd003ba47c8
mkdir -p "$work_root"

validation_commit=$(git -C "$branch_root" rev-parse HEAD)
args=(
    --source "$source_root"
    --work "$work_root"
    --target x86_64-unknown-linux-gnu
    --version 0.1.9-linux103-candidate
    --expected-commit 009d33f85cb7ac087d0c25add1161dd003ba47c8
    --validation-commit "$validation_commit"
)
if [ "$mode" = debian ]; then
    args+=(--container debian:12)
fi

bash "$branch_root/.github/scripts/linux103-candidate-evidence.sh" \
    "${args[@]}"
