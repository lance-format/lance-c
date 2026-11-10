#!/usr/bin/env bash
set -u -o pipefail

SOURCE_COMMIT="8b6adf90a1c6604438c40ac9e3056e9bb9fab214"
SCRIPT_PATH="validation/validate-native-version-tags.sh"

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <evidence-dir>" >&2
    exit 2
fi

evidence_dir="$1"
mkdir -p "$evidence_dir"

write_atomic() {
    local path="$1"
    local value="$2"
    printf '%s\n' "$value" > "$path.tmp"
    mv "$path.tmp" "$path"
}

fail_setup() {
    local message="$1"
    printf '%s\n' "$message" >&2
    write_atomic "$evidence_dir/setup.status" 1
    exit 1
}

rm -f "$evidence_dir"/*.status "$evidence_dir"/*.status.tmp

head_commit=$(git rev-parse HEAD) || fail_setup "failed to read HEAD"
if ! git merge-base --is-ancestor "$SOURCE_COMMIT" "$head_commit"; then
    fail_setup "source commit $SOURCE_COMMIT is not an ancestor of $head_commit"
fi

unexpected=$(git diff --name-only "$SOURCE_COMMIT" "$head_commit" | grep -v '^validation/' || true)
if [ -n "$unexpected" ]; then
    fail_setup "validation branch changes implementation files: $unexpected"
fi

write_atomic "$evidence_dir/source-commit" "$SOURCE_COMMIT"
write_atomic "$evidence_dir/validation-head" "$head_commit"
write_atomic "$evidence_dir/host" "$(hostname)"
write_atomic "$evidence_dir/uname" "$(uname -a)"
write_atomic "$evidence_dir/rustc" "$(rustc +1.94.0 --version)"
write_atomic "$evidence_dir/cargo" "$(cargo +1.94.0 --version)"
write_atomic "$evidence_dir/pwd" "$(pwd)"
write_atomic "$evidence_dir/setup.status" 0

export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

run_step() {
    local name="$1"
    shift

    printf '== %s ==\n' "$name"
    local log="$evidence_dir/$name.log"
    "$@" > "$log" 2>&1
    local status=$?
    write_atomic "$evidence_dir/$name.status" "$status"
    if [ "$status" -ne 0 ]; then
        printf '%s failed with status %s; see %s\n' "$name" "$status" "$log" >&2
        write_atomic "$evidence_dir/overall.status" "$status"
        exit "$status"
    fi
    printf '%s: OK\n' "$name"
}

run_step fmt cargo +1.94.0 fmt --check
run_step check cargo +1.94.0 check --all-targets
run_step clippy cargo +1.94.0 clippy --all-targets -- -D warnings
run_step test cargo +1.94.0 test
run_step native cargo +1.94.0 test --test compile_and_run_test -- --ignored

write_atomic "$evidence_dir/overall.status" 0
printf 'all validation steps passed\n'
