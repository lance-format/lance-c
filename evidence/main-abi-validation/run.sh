#!/usr/bin/env bash
# Validation runner for the public C ABI regression test.
#
# This file lives on a task-specific validation branch. It is not part of the
# product branch and exists only to make the Nier run reproducible.
set -uo pipefail

SRC=${SRC:?set SRC to the exact source checkout}
EV=${EV:?set EV to the evidence directory}
MODE=${1:-green}

BASE_COMMIT=d1c0c36ebf11f3fc042bdc7faf9edafd192c6736
BASE_TREE=474a86230949a0736805ff1e0d9c2545a69b2be5

export RUSTUP_TOOLCHAIN=1.94.0
export CARGO_BUILD_JOBS=8
export CARGO_TERM_COLOR=never
export RUST_BACKTRACE=0

mkdir -p "$EV/logs" "$EV/control-patches"
: > "$EV/steps.tsv"
: > "$EV/runner.log"

log_line() {
    printf '%s\n' "$*" | tee -a "$EV/runner.log"
}

cd "$SRC" || exit 1

cleanup_controls() {
    git checkout -- . >/dev/null 2>&1 || true
}
trap cleanup_controls EXIT

verify_source() {
    local head tree status
    head=$(git rev-parse HEAD)
    tree=$(git rev-parse HEAD^{tree})
    status=$(git status --porcelain=v1)
    {
        echo "commit=$head"
        echo "tree=$tree"
        if [ -n "$status" ]; then
            echo "dirty=yes"
            printf '%s\n' "$status"
        else
            echo "dirty=no"
        fi
    } | tee "$EV/source-verify.txt"

    if [ "$head" != "$BASE_COMMIT" ] || [ "$tree" != "$BASE_TREE" ] || [ -n "$status" ]; then
        log_line "SOURCE VERIFY FAILED"
        return 1
    fi
}

write_status() {
    local result="$1"
    local failed_step="$2"
    local tmp="$EV/final.status.json.tmp.$$"

    cat > "$tmp" <<EOF
{
  "result": "$result",
  "failed_step": "$failed_step",
  "commit": "$(git rev-parse HEAD)",
  "tree": "$(git rev-parse HEAD^{tree})",
  "workdir": "$SRC",
  "toolchain": "1.94.0",
  "cargo_build_jobs": 8,
  "test_threads": 4,
  "evidence_dir": "$EV",
  "finished_at": "$(date -Is)"
}
EOF
    mv -f "$tmp" "$EV/final.status.json"
    printf '%s\n' "$result" > "$EV/result.status"
}

run_step() {
    local name="$1"
    shift
    local log="$EV/logs/$name.log"
    local start finish rc result

    start=$(date -Is)
    log_line "BEGIN $name :: $*"
    printf '$ %s\n' "$*" > "$log"
    "$@" >> "$log" 2>&1
    rc=$?
    finish=$(date -Is)

    if [ "$rc" -eq 0 ]; then
        result=PASS
    else
        result=FAIL
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$result" "$rc" "$start" "$finish" "$*" >> "$EV/steps.tsv"
    log_line "END $name rc=$rc $result"
    return "$rc"
}

require_one_passed() {
    local log="$1"
    if ! grep -Eq '^test result: ok\. [1-9][0-9]* passed;' "$log"; then
        log_line "NO TEST RAN: $log"
        return 1
    fi
}

run_expect_failure() {
    local name="$1"
    local expected="$2"
    shift 2
    local log="$EV/logs/$name.log"
    local start finish rc

    start=$(date -Is)
    printf '$ %s\n' "$*" > "$log"
    "$@" >> "$log" 2>&1
    rc=$?
    finish=$(date -Is)

    if [ "$rc" -eq 0 ]; then
        log_line "CONTROL FAIL $name: test passed unexpectedly"
        return 1
    fi
    if ! grep -Fq "$expected" "$log"; then
        log_line "CONTROL FAIL $name: missing expected diagnostic: $expected"
        grep -nE 'ABI|field-offset|enum-value|manifest|mismatch' "$log" | tail -n 20 || true
        return 1
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "FAIL_AS_EXPECTED" "$rc" "$start" "$finish" \
        "negative control" >> "$EV/steps.tsv"
    log_line "CONTROL PASS $name rc=$rc"
}

run_green() {
    run_step 01-fmt cargo +1.94.0 fmt --check || { write_status FAIL 01-fmt; exit 10; }
    run_step 02-check cargo +1.94.0 check --all-targets || { write_status FAIL 02-check; exit 11; }
    run_step 03-clippy cargo +1.94.0 clippy --all-targets -- -D warnings || { write_status FAIL 03-clippy; exit 12; }
    run_step 04-scanner cargo +1.94.0 test --test compile_and_run_test header_scanner_recognizes_public_declarations || { write_status FAIL 04-scanner; exit 13; }
    require_one_passed "$EV/logs/04-scanner.log" || { write_status FAIL 04-scanner-no-test; exit 14; }

    run_step 05-abi cargo +1.94.0 test --test compile_and_run_test -- --ignored --test-threads=4 test_public_c_abi_layout_matches_rust --exact --nocapture || { write_status FAIL 05-abi; exit 15; }
    require_one_passed "$EV/logs/05-abi.log" || { write_status FAIL 05-abi-no-test; exit 16; }

    run_step 06-c-consumer cargo +1.94.0 test --test compile_and_run_test -- --ignored --test-threads=4 test_c_compilation_and_execution --exact || { write_status FAIL 06-c-consumer; exit 17; }
    require_one_passed "$EV/logs/06-c-consumer.log" || { write_status FAIL 06-c-consumer-no-test; exit 18; }

    run_step 07-cpp-consumer cargo +1.94.0 test --test compile_and_run_test -- --ignored --test-threads=4 test_cpp_compilation_and_execution --exact || { write_status FAIL 07-cpp-consumer; exit 19; }
    require_one_passed "$EV/logs/07-cpp-consumer.log" || { write_status FAIL 07-cpp-consumer-no-test; exit 20; }
}

apply_control() {
    local name="$1"
    python3 - "$name" "$SRC" <<'PY'
import sys
from pathlib import Path

name = sys.argv[1]
root = Path(sys.argv[2])

def replace_once(path, old, new):
    target = root / path
    text = target.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    target.write_text(text.replace(old, new))

if name == "panic-discriminant":
    replace_once(
        "include/lance/lance.h",
        "    LANCE_ERR_PANIC = 9,",
        "    LANCE_ERR_PANIC = 10,",
    )
elif name == "swap-enum-values":
    replace_once(
        "include/lance/lance.h",
        "    LANCE_ERR_IO = 2,\n    LANCE_ERR_NOT_FOUND = 3,",
        "    LANCE_ERR_IO = 3,\n    LANCE_ERR_NOT_FOUND = 2,",
    )
elif name == "swap-same-typed-fields":
    replace_once(
        "include/lance/lance.h",
        "    uint64_t index_cache_hits;\n    uint64_t index_cache_misses;",
        "    uint64_t index_cache_misses;\n    uint64_t index_cache_hits;",
    )
elif name == "enum-repr-layout":
    replace_once(
        "src/error.rs",
        "#[repr(C)]\n#[derive(Clone, Copy, Debug, PartialEq, Eq)]\npub enum LanceErrorCode",
        "#[repr(u64)]\n#[derive(Clone, Copy, Debug, PartialEq, Eq)]\npub enum LanceErrorCode",
    )
elif name == "new-unmapped-enumerator":
    replace_once(
        "include/lance/lance.h",
        "    LANCE_ERR_PANIC = 9,\n} LanceErrorCode;",
        "    LANCE_ERR_PANIC = 9,\n    LANCE_ERR_NEW = 10,\n} LanceErrorCode;",
    )
else:
    raise SystemExit(f"unknown control: {name}")
PY
}

run_control() {
    local name="$1"
    local expected="$2"

    apply_control "$name" || {
        log_line "CONTROL SETUP FAIL $name"
        write_status FAIL "control-setup-$name"
        exit 30
    }
    git diff > "$EV/control-patches/$name.patch"

    local test_args=(
        cargo +1.94.0 test --test compile_and_run_test --
        --ignored --test-threads=1
        test_public_c_abi_layout_matches_rust --exact --nocapture
    )
    run_expect_failure "control-$name" "$expected" "${test_args[@]}" || {
        write_status FAIL "control-$name"
        exit 31
    }

    git checkout -- .
    if [ -n "$(git status --porcelain=v1)" ]; then
        log_line "CONTROL CLEANUP FAIL $name"
        write_status FAIL "control-cleanup-$name"
        exit 32
    fi
}

run_controls() {
    run_control "panic-discriminant" "LANCE_ERR_PANIC"
    run_control "swap-enum-values" "enum-value|LanceErrorCode|LANCE_ERR_IO"
    run_control "swap-same-typed-fields" "field-offset|LanceSessionCacheStats|index_cache_hits"
    run_control "enum-repr-layout" "enum-size|LanceErrorCode"
    run_control "new-unmapped-enumerator" "is in the public header but not in the ABI manifest"

    if ! verify_source; then
        write_status FAIL post-control-source
        exit 33
    fi

    run_step 08-abi-after-controls cargo +1.94.0 test --test compile_and_run_test -- --ignored --test-threads=4 test_public_c_abi_layout_matches_rust --exact --nocapture || { write_status FAIL 08-abi-after-controls; exit 34; }
    require_one_passed "$EV/logs/08-abi-after-controls.log" || { write_status FAIL 08-abi-after-controls-no-test; exit 35; }
}

{
    echo "run_started=$(date -Is)"
    echo "mode=$MODE"
    echo "workdir=$SRC"
    echo "commit=$(git rev-parse HEAD)"
    echo "tree=$(git rev-parse HEAD^{tree})"
    echo "RUSTUP_TOOLCHAIN=$RUSTUP_TOOLCHAIN"
    echo "CARGO_BUILD_JOBS=$CARGO_BUILD_JOBS"
    echo "rustc=$(rustc --version)"
    echo "cargo=$(cargo --version)"
    echo "clang=$(clang --version | head -n 1)"
} | tee "$EV/run-env.txt"

if ! verify_source; then
    write_status FAIL source-verify
    exit 2
fi

case "$MODE" in
    green)
        run_green
        write_status PASS ""
        log_line "GREEN PASS $(date -Is)"
        ;;
    controls)
        run_controls
        write_status PASS "negative-controls"
        log_line "CONTROLS PASS $(date -Is)"
        ;;
    *)
        log_line "unknown mode: $MODE"
        write_status FAIL "unknown-mode"
        exit 3
        ;;
esac
