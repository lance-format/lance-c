#!/usr/bin/env bash
# Bounded RED/GREEN execution for the native last_as_prefix public-method
# compatibility repair. One mode per invocation; a mode whose test was already
# attempted refuses to run again.
#
# Usage: run-public-expansion-compat.sh <red|green>
#
# The artifact repository is the Git checkout containing this script. The
# pinned Lance source checkout and evidence root default to task-specific
# directories under ~/Code/validation and can be overridden with
# PREFIX_COMPAT_SRC and PREFIX_COMPAT_EV.
set -uo pipefail
umask 022

mode=${1:-}
if [[ $mode != red && $mode != green ]]; then
    echo "usage: $0 red|green" >&2
    exit 64
fi

artifact_repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
src=${PREFIX_COMPAT_SRC:-$HOME/Code/validation/lance-native-prefix-40f15ed-public-expansion-compat}
ev_root=${PREFIX_COMPAT_EV:-$HOME/Code/validation/lance-prefix-public-expansion-compat-evidence}
out=$ev_root/$mode
mkdir -p "$out"
exec >>"$out/outer.log" 2>&1

base=40f15ed1b71b66950961b175829fdf17a763f108
oracle_rel=.validation/last-as-prefix/fts_last_as_prefix.rs
patch_rel=.validation/last-as-prefix/native-last-as-prefix.patch
oracle_sha_expected=1437e3aeb14215dfe1b6673ed05147f109fe82f0d8db1b14d7936bf2b8c0b690
oracle_git_blob_expected=88aff37f96721265409834c6bc75c1059118d1fb

case $mode in
    red)
        artifact_commit=51d4fddc53c60ccb4633b06116205a5c92298e99
        patch_sha_expected=3cdaf8373790c5e952585ab85288c9719859cfce2ad06645d3904e0821ab6d4a
        test_filter=(public_exact_vocabulary_expansion_filters_and_caps_tokens -- --exact --test-threads=1 --nocapture)
        ;;
    green)
        artifact_commit=fc30d79e8dd70de844a2f3bf5621468be69b1e46
        patch_sha_expected=18bad6fe2ae452dc7bd91e5243385eccb9a749413b63f8be8533afab45ae720e
        test_filter=(-- --test-threads=4)
        ;;
esac

command_line="cargo +1.94.0 test --locked -p lance --test fts_last_as_prefix ${test_filter[*]}"

status_written=0
head_before=
head_after=
patch_sha_before=
patch_sha_after=
oracle_sha_before=
oracle_sha_after=
applied_diff_sha256=
applied_diff_stat=
cargo_rc=
running_line=
result_line=
test_binary=
test_binary_sha256=
classification=
first_failure=
detail=

write_status() {
    local class=$1 code=$2 note=${3:-}
    local tmp="$out/status.json.tmp"
    jq -n \
        --arg mode "$mode" \
        --arg artifact_repo "$artifact_repo" \
        --arg artifact_commit "$artifact_commit" \
        --arg source_repo "$src" \
        --arg source_head_before "$head_before" \
        --arg source_head_after "$head_after" \
        --arg patch_sha256_expected "$patch_sha_expected" \
        --arg patch_sha256_before "$patch_sha_before" \
        --arg patch_sha256_after "$patch_sha_after" \
        --arg oracle_sha256_expected "$oracle_sha_expected" \
        --arg oracle_sha256_before "$oracle_sha_before" \
        --arg oracle_sha256_after "$oracle_sha_after" \
        --arg oracle_git_blob_expected "$oracle_git_blob_expected" \
        --arg applied_diff_sha256 "$applied_diff_sha256" \
        --arg applied_diff_stat "$applied_diff_stat" \
        --arg command "$command_line" \
        --arg rustup_toolchain "${RUSTUP_TOOLCHAIN:-}" \
        --arg cargo_build_jobs "${CARGO_BUILD_JOBS:-}" \
        --arg rust_test_threads "${RUST_TEST_THREADS:-}" \
        --arg cargo_version "${cargo_version:-}" \
        --arg cargo_path "${cargo_path:-}" \
        --arg rustc_version "${rustc_version:-}" \
        --arg rustc_path "${rustc_path:-}" \
        --arg binary "$test_binary" \
        --arg binary_sha256 "$test_binary_sha256" \
        --arg running "$running_line" \
        --arg test_result "$result_line" \
        --arg exit_code "$code" \
        --arg classification "$class" \
        --arg first_failure "$first_failure" \
        --arg detail "$note" \
        --arg log "$out/runner.log" \
        --arg environment "$out/environment.txt" \
        --arg outer_log "$out/outer.log" \
        --arg finished_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        '{
            mode: $mode,
            artifact_repo: $artifact_repo,
            artifact_commit: $artifact_commit,
            source_repo: $source_repo,
            source_head_before: $source_head_before,
            source_head_after: $source_head_after,
            patch_sha256_expected: $patch_sha256_expected,
            patch_sha256_before: $patch_sha256_before,
            patch_sha256_after: $patch_sha256_after,
            oracle_sha256_expected: $oracle_sha256_expected,
            oracle_sha256_before: $oracle_sha256_before,
            oracle_sha256_after: $oracle_sha256_after,
            oracle_git_blob_expected: $oracle_git_blob_expected,
            applied_diff_sha256: $applied_diff_sha256,
            applied_diff_stat: $applied_diff_stat,
            command: $command,
            environment: {
                rustup_toolchain: $rustup_toolchain,
                cargo_build_jobs: $cargo_build_jobs,
                rust_test_threads: $rust_test_threads,
                cargo_version: $cargo_version,
                cargo_path: $cargo_path,
                rustc_version: $rustc_version,
                rustc_path: $rustc_path
            },
            test_binary: $binary,
            test_binary_sha256: $binary_sha256,
            running_line: $running,
            test_result_line: $test_result,
            exit_code: ($exit_code | tonumber),
            classification: $classification,
            first_failure: $first_failure,
            detail: $detail,
            log: $log,
            environment_log: $environment,
            outer_log: $outer_log,
            finished_at: $finished_at
        }' >"$tmp" && mv "$tmp" "$out/status.json"
    status_written=1
}

die() {
    detail=$*
    echo "ERROR: $*" >&2
    write_status "setup_failure" 2 "$*" || true
    exit 2
}

on_exit() {
    local rc=$?
    trap - EXIT
    if [[ $status_written -eq 0 ]]; then
        write_status "runner_aborted" "$rc" "runner exited before writing status" || true
    fi
    printf '%s\n' "$rc" >"$out/outer.exit.tmp" && mv "$out/outer.exit.tmp" "$out/outer.exit"
    exit "$rc"
}
trap on_exit EXIT

if [[ -e $out/test-attempted ]]; then
    die "test already attempted for mode $mode; refusing to rerun"
fi

echo "mode=$mode started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "artifact_repo=$artifact_repo source_repo=$src evidence=$out"

export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export PATH="$HOME/.local/share/mbx/bin:$HOME/.cargo/bin:$PATH"

git -C "$artifact_repo" cat-file -e "$artifact_commit^{commit}" || die "artifact commit unavailable"
[[ $(git -C "$artifact_repo" rev-parse "$artifact_commit") == "$artifact_commit" ]] || die "artifact commit mismatch"

head_before=$(git -C "$src" rev-parse HEAD) || die "cannot read source HEAD"
[[ $head_before == "$base" ]] || die "unexpected base HEAD $head_before (expected $base)"
[[ -z $(git -C "$src" status --porcelain) ]] || die "source checkout is not clean"

mkdir -p "$out/artifacts"
git -C "$artifact_repo" show "$artifact_commit:$oracle_rel" >"$out/artifacts/fts_last_as_prefix.rs" || die "cannot extract oracle"
git -C "$artifact_repo" show "$artifact_commit:$patch_rel" >"$out/artifacts/native-last-as-prefix.patch" || die "cannot extract patch"

patch_sha_before=$(sha256sum "$out/artifacts/native-last-as-prefix.patch" | cut -d' ' -f1)
oracle_sha_before=$(sha256sum "$out/artifacts/fts_last_as_prefix.rs" | cut -d' ' -f1)
oracle_blob=$(git -C "$src" hash-object "$out/artifacts/fts_last_as_prefix.rs")
[[ $patch_sha_before == "$patch_sha_expected" ]] || die "patch sha256 $patch_sha_before != $patch_sha_expected"
[[ $oracle_sha_before == "$oracle_sha_expected" ]] || die "oracle sha256 $oracle_sha_before != $oracle_sha_expected"
[[ $oracle_blob == "$oracle_git_blob_expected" ]] || die "oracle git blob $oracle_blob != $oracle_git_blob_expected"

cargo_path=$(command -v cargo) || die "cargo not found"
cargo_version=$(cargo +1.94.0 --version) || die "cargo +1.94.0 unavailable"
rustc_path=$(rustup which --toolchain 1.94.0 rustc) || die "rustc 1.94.0 unavailable"
rustc_version=$(rustc +1.94.0 --version) || die "rustc +1.94.0 unavailable"

{
    printf 'mode=%s\n' "$mode"
    printf 'started_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'artifact_repo=%s\n' "$artifact_repo"
    printf 'artifact_commit=%s\n' "$artifact_commit"
    printf 'source_repo=%s\n' "$src"
    printf 'source_head=%s\n' "$head_before"
    printf 'RUSTUP_TOOLCHAIN=%s\n' "$RUSTUP_TOOLCHAIN"
    printf 'CARGO_BUILD_JOBS=%s\n' "$CARGO_BUILD_JOBS"
    printf 'RUST_TEST_THREADS=%s\n' "$RUST_TEST_THREADS"
    printf 'cargo_path=%s\n' "$cargo_path"
    printf 'cargo_version=%s\n' "$cargo_version"
    printf 'rustc_path=%s\n' "$rustc_path"
    printf 'rustc_version=%s\n' "$rustc_version"
    printf 'rustfmt_version=%s\n' "$(rustfmt +1.94.0 --version)"
    printf 'host=%s\n' "$(hostname)"
    printf 'kernel=%s\n' "$(uname -sr)"
    printf 'nproc=%s\n' "$(nproc)"
    printf 'patch_sha256=%s\n' "$patch_sha_before"
    printf 'oracle_sha256=%s\n' "$oracle_sha_before"
    printf 'oracle_git_blob=%s\n' "$oracle_blob"
    printf 'command=%s\n' "$command_line"
} >"$out/environment.txt"

{
    printf 'source_head=%s\n' "$head_before"
    printf 'artifact_commit=%s\n' "$artifact_commit"
    printf 'patch_sha256=%s\n' "$patch_sha_before"
    printf 'oracle_sha256=%s\n' "$oracle_sha_before"
    printf 'oracle_git_blob=%s\n' "$oracle_blob"
    printf 'source_status=%s\n' "$(git -C "$src" status --porcelain | tr '\n' ';')"
} >"$out/hashes-before.txt"

rustfmt +1.94.0 --check --edition 2024 "$out/artifacts/fts_last_as_prefix.rs" >"$out/rustfmt-check.txt" 2>&1 \
    || die "rustfmt --check failed for oracle under edition 2024; see $out/rustfmt-check.txt"

git -C "$src" apply --check --whitespace=error-all "$out/artifacts/native-last-as-prefix.patch" \
    || die "git apply --check failed"

git -C "$src" apply --whitespace=error-all "$out/artifacts/native-last-as-prefix.patch" \
    || die "git apply failed"
cp "$out/artifacts/fts_last_as_prefix.rs" "$src/rust/lance/tests/fts_last_as_prefix.rs" \
    || die "cannot install oracle into source checkout"

git -C "$src" diff --check || die "applied source diff has whitespace errors"
git -C "$src" diff >"$out/applied.diff"
git -C "$src" diff --stat >"$out/applied-stat.txt"
git -C "$src" status --porcelain >"$out/source-status-applied.txt"
applied_diff_sha256=$(sha256sum "$out/applied.diff" | cut -d' ' -f1)
applied_diff_stat=$(git -C "$src" diff --stat | tail -1)

[[ $(sha256sum "$src/rust/lance/tests/fts_last_as_prefix.rs" | cut -d' ' -f1) == "$oracle_sha_expected" ]] \
    || die "installed oracle hash mismatch"
git -C "$src" apply --reverse --check "$out/artifacts/native-last-as-prefix.patch" \
    || die "applied patch cannot be reversed cleanly"

printf 'attempted_at=%s\ncommand=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$command_line" >"$out/test-attempted.tmp"
mv "$out/test-attempted.tmp" "$out/test-attempted"

echo "running: $command_line"
set +e
(
    cd "$src" || exit 1
    cargo +1.94.0 test --locked -p lance --test fts_last_as_prefix "${test_filter[@]}"
) 2>&1 | tee "$out/runner.log"
cargo_rc=${PIPESTATUS[0]}
set +e

running_line=$(grep -E '^running [0-9]+ tests?$' "$out/runner.log" | tail -1)
result_line=$(grep -E '^test result:' "$out/runner.log" | tail -1)
test_binary=$(grep -E '^ +Running tests/fts_last_as_prefix.rs \(' "$out/runner.log" | sed -E 's/^[^(]*\((.*)\)$/\1/' | tail -1)
if [[ -n $test_binary && $test_binary != /* ]]; then
    test_binary=$src/$test_binary
fi
if [[ -n $test_binary && -f $test_binary ]]; then
    test_binary_sha256=$(sha256sum "$test_binary" | cut -d' ' -f1)
fi

head_after=$(git -C "$src" rev-parse HEAD)
patch_sha_after=$(git -C "$artifact_repo" show "$artifact_commit:$patch_rel" | sha256sum | cut -d' ' -f1)
oracle_sha_after=$(git -C "$artifact_repo" show "$artifact_commit:$oracle_rel" | sha256sum | cut -d' ' -f1)

{
    printf 'source_head=%s\n' "$head_after"
    printf 'artifact_commit=%s\n' "$artifact_commit"
    printf 'patch_sha256=%s\n' "$patch_sha_after"
    printf 'oracle_sha256=%s\n' "$oracle_sha_after"
    printf 'applied_diff_sha256=%s\n' "$applied_diff_sha256"
    printf 'source_status=%s\n' "$(git -C "$src" status --porcelain | tr '\n' ';')"
    printf 'runner_exit=%s\n' "$cargo_rc"
} >"$out/hashes-after.txt"

first_failure=$(grep -B2 -A10 -E 'assertion .* failed|panicked at' "$out/runner.log" | head -30)

if [[ $mode == red ]]; then
    if [[ $cargo_rc -eq 101 ]] \
        && [[ $running_line == "running 1 test" ]] \
        && [[ $result_line == *"0 passed; 1 failed"* ]] \
        && grep -q "exact vocabulary cap=8" "$out/runner.log" \
        && grep -q "assertion" "$out/runner.log"; then
        classification=valid_red_assertion
    elif grep -q "could not compile" "$out/runner.log"; then
        classification=invalid_red_compile
    else
        classification=invalid_red_other
    fi
else
    if [[ $cargo_rc -eq 0 ]] \
        && [[ $running_line == "running 9 tests" ]] \
        && [[ $result_line == *"9 passed; 0 failed"* ]]; then
        classification=valid_green
    elif grep -q "could not compile" "$out/runner.log"; then
        classification=invalid_green_compile
    else
        classification=invalid_green_other
    fi
fi

write_status "$classification" "$cargo_rc"
echo "classification=$classification exit=$cargo_rc"
exit "$cargo_rc"
