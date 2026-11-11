#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <stage-prefix> <evidence-dir>" >&2
    exit 2
fi

stage=$(cd "$1" && pwd)
evidence=$2
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dataset="$repo_root/test_data/v0.27.1/pq_in_schema"
work="$evidence/validation"
mkdir -p "$work/logs" "$work/unexpected/lib" "$work/ssl/lib" "$work/malformed/lib" \
    "$work/truncated/lib" "$work/missing/lib" "$work/wrapper"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

expect_failure() {
    description=$1
    pattern=$2
    shift 2
    log="$work/logs/$description.log"
    if "$@" >"$log" 2>&1; then
        echo "expected failure: $description" >&2
        cat "$log" >&2
        exit 1
    fi
    grep -q "$pattern" "$log" || {
        echo "missing expected diagnostic for $description: $pattern" >&2
        cat "$log" >&2
        exit 1
    }
    echo "PASS negative: $description"
}

cat >"$work/fixture.c" <<'C'
int fixture_symbol(void) { return 0; }
C

cc -shared -fPIC -Wl,-soname,liblance_c.so -o "$work/unexpected/lib/liblance_c.so" \
    "$work/fixture.c" -Wl,--no-as-needed -l:libz.so.1
cc -shared -fPIC -Wl,-soname,liblance_c.so -o "$work/ssl/lib/liblance_c.so" \
    "$work/fixture.c" -Wl,--no-as-needed -l:libssl.so.3
printf 'not an ELF file\n' >"$work/malformed/lib/liblance_c.so"
head -c 64 "$stage/lib/liblance_c.so" >"$work/truncated/lib/liblance_c.so"

echo "==> Positive staged runtime dependency guard"
bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$stage" x86_64-unknown-linux-gnu \
    | tee "$work/logs/positive-dt-needed.log"

echo "==> Positive shared export guard"
bash "$repo_root/.github/scripts/verify-shared-exports.sh" \
    "$stage/lib/liblance_c.so" "$stage/include/lance/lance.h" linux \
    | tee "$work/logs/positive-exports.log"

echo "==> Negative staged runtime dependency guards"
expect_failure "unexpected" "unexpected DT_NEEDED.*libz.so.1" \
    bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$work/unexpected" x86_64-unknown-linux-gnu
expect_failure "ssl" "forbidden TLS/crypto DT_NEEDED.*libssl.so.3" \
    bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$work/ssl" x86_64-unknown-linux-gnu
expect_failure "malformed" "not a readable ELF file" \
    bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$work/malformed" x86_64-unknown-linux-gnu
expect_failure "truncated" "no dynamic section" \
    bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$work/truncated" x86_64-unknown-linux-gnu
expect_failure "missing" "staged shared library is missing" \
    bash "$repo_root/.github/scripts/verify-linux-staging.sh" "$work/missing" x86_64-unknown-linux-gnu

echo "==> macOS linker shim argument rewriting"
printf '_old\n' >"$work/wrapper/list"
LANCE_C_MACOS_LINKER=/bin/true bash "$repo_root/.github/scripts/limit-macos-exports.sh" \
    -o "$work/wrapper/liblance_c.dylib" \
    -Wl,-dead_strip,-exported_symbols_list,"$work/wrapper/list"
grep -qx '_lance_\*' "$work/wrapper/list" || fail "linker shim did not rewrite the export list"
echo "PASS: linker shim rewrote the liblance_c.dylib export list"

cat >"$work/c_consumer.c" <<'C'
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <lance/lance.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    LanceDataset *dataset = lance_dataset_open(argv[1], NULL, 0);
    if (dataset == NULL) {
        const char *message = lance_last_error_message();
        fprintf(stderr, "lance error: %s\n", message != NULL ? message : "unknown");
        lance_free_string(message);
        return 3;
    }
    uint64_t rows = lance_dataset_count_rows(dataset);
    uint64_t version = lance_dataset_version(dataset);
    printf("rows=%" PRIu64 " version=%" PRIu64 "\n", rows, version);
    lance_dataset_close(dataset);
    return rows == 512 && version == 2 ? 0 : 4;
}
C

cat >"$work/dlopen_consumer.c" <<'C'
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <lance/lance.h>

typedef LanceDataset *(*open_fn)(const char *, const char *const *, uint64_t);
typedef uint64_t (*count_fn)(const LanceDataset *);
typedef uint64_t (*version_fn)(const LanceDataset *);
typedef void (*close_fn)(LanceDataset *);

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 3;
    }
    open_fn open_dataset = (open_fn)dlsym(handle, "lance_dataset_open");
    count_fn count_rows = (count_fn)dlsym(handle, "lance_dataset_count_rows");
    version_fn version = (version_fn)dlsym(handle, "lance_dataset_version");
    close_fn close_dataset = (close_fn)dlsym(handle, "lance_dataset_close");
    if (open_dataset == NULL || count_rows == NULL || version == NULL || close_dataset == NULL) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        dlclose(handle);
        return 4;
    }
    LanceDataset *dataset = open_dataset(argv[2], NULL, 0);
    if (dataset == NULL) {
        fprintf(stderr, "open failed\n");
        dlclose(handle);
        return 5;
    }
    uint64_t rows = count_rows(dataset);
    uint64_t ds_version = version(dataset);
    printf("dlopen rows=%" PRIu64 " version=%" PRIu64 "\n", rows, ds_version);
    close_dataset(dataset);
    dlclose(handle);
    return rows == 512 && ds_version == 2 ? 0 : 6;
}
C

echo "==> C consumer against installed shared and static libraries"
cc -I"$stage/include" "$work/c_consumer.c" -L"$stage/lib" -Wl,-rpath,"$stage/lib" \
    -llance_c -o "$work/c_consumer_shared"
"$work/c_consumer_shared" "$dataset" | tee "$work/logs/c-shared.log"
cat "$work/logs/c-shared.log" | grep -qx 'rows=512 version=2' || fail "shared C consumer data mismatch"

cc -I"$stage/include" "$work/c_consumer.c" "$stage/lib/liblance_c.a" \
    -lpthread -ldl -lm -o "$work/c_consumer_static"
"$work/c_consumer_static" "$dataset" | tee "$work/logs/c-static.log"
cat "$work/logs/c-static.log" | grep -qx 'rows=512 version=2' || fail "static C consumer data mismatch"

echo "==> dlopen consumer"
cc -I"$stage/include" "$work/dlopen_consumer.c" -ldl -o "$work/dlopen_consumer"
"$work/dlopen_consumer" "$stage/lib/liblance_c.so" "$dataset" | tee "$work/logs/dlopen.log"
cat "$work/logs/dlopen.log" | grep -qx 'dlopen rows=512 version=2' || fail "dlopen consumer data mismatch"

for linkage in static shared; do
    echo "==> CMake consumer ($linkage)"
    cmake -S "$repo_root/examples/cmake-consumer" -B "$work/consumer-$linkage" \
        -DCMAKE_PREFIX_PATH="$stage" -DCMAKE_BUILD_TYPE=Release -DLanceC_LINK="$linkage" \
        >"$work/logs/cmake-$linkage.log" 2>&1
    cmake --build "$work/consumer-$linkage" --parallel 1 >>"$work/logs/cmake-$linkage.log" 2>&1
    if [ "$linkage" = "shared" ]; then
        LD_LIBRARY_PATH="$stage/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$work/consumer-$linkage/consumer" "$dataset" | tee "$work/logs/cmake-$linkage-run.log"
    else
        "$work/consumer-$linkage/consumer" "$dataset" | tee "$work/logs/cmake-$linkage-run.log"
    fi
    cat "$work/logs/cmake-$linkage-run.log" | grep -qx 'rows: 512, version: 2' ||
        fail "CMake $linkage consumer data mismatch"
done

echo "PASS: all release artifact validations"
