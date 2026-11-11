#!/usr/bin/env bash
set -uo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=${1:?usage: run_all.sh <repo> <out>}
out=${2:?usage: run_all.sh <repo> <out>}

mkdir -p "$out/logs"
export PATH="$HOME/.local/bin:$PATH"
export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

cd "$repo"
started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
head=$(git rev-parse HEAD)
tree=$(git rev-parse 'HEAD^{tree}')
branch=$(git rev-parse --abbrev-ref HEAD)
status_before=$(git status --porcelain=v1)
rustc_version=$(rustc --version)
cargo_version=$(cargo --version)

{
    printf 'started_at=%s\n' "$started_at"
    printf 'hostname=%s\n' "$(hostname)"
    printf 'user=%s\n' "$(whoami)"
    printf 'repo=%s\n' "$repo"
    printf 'branch=%s\n' "$branch"
    printf 'head=%s\n' "$head"
    printf 'tree=%s\n' "$tree"
    printf 'rustup_toolchain=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'rustc=%s\n' "$rustc_version"
    printf 'cargo=%s\n' "$cargo_version"
    printf 'cargo_build_jobs=%s\n' "$CARGO_BUILD_JOBS"
    printf 'rust_test_threads=%s\n' "$RUST_TEST_THREADS"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'path=%s\n' "$PATH"
    printf 'status_before=%s\n' "$status_before"
} >"$out/meta.tmp"
mv "$out/meta.tmp" "$out/meta.txt"

status_file="$out/status.tmp"
printf 'phase\tstatus_rc\tstarted_at\tfinished_at\n' >"$status_file"
failed=0

for phase in fmt check clippy focused full ignored; do
    phase_started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    if bash "$script_dir/run_phase.sh" "$phase" "$repo" "$out"; then
        rc=0
    else
        rc=$?
        failed=1
    fi
    phase_finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    printf '%s\t%s\t%s\t%s\n' "$phase" "$rc" "$phase_started" "$phase_finished" >>"$status_file"
done

status_after=$(git status --porcelain=v1)
{
    printf 'finished_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'status_after=%s\n' "$status_after"
    printf 'failed=%s\n' "$failed"
} >>"$out/meta.txt"
mv "$status_file" "$out/status.tsv"
printf '%s\n' "$failed" >"$out/exit_code.tmp"
mv "$out/exit_code.tmp" "$out/exit_code"

if [[ "$failed" -ne 0 ]]; then
    printf 'validation failed; see %s\n' "$out" >&2
    exit 1
fi
printf 'validation passed; see %s\n' "$out"
