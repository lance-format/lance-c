#!/usr/bin/env bash
set -uo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=${1:?usage: launch_tmux.sh <repo> <out> <channel>}
out=${2:?usage: launch_tmux.sh <repo> <out> <channel>}
channel=${3:?usage: launch_tmux.sh <repo> <out> <channel>}

mkdir -p "$out"
finish() {
    local rc=$?
    printf '%s\n' "$rc" >"$out/launcher_exit_code.tmp"
    mv "$out/launcher_exit_code.tmp" "$out/launcher_exit_code"
    tmux wait-for -S "$channel"
}
trap finish EXIT

cd "$repo"
bash "$script_dir/run_all.sh" "$repo" "$out"
