#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Supplementary probes for the PR #50 release path that the main run does not
# settle on its own:
#
#  1. an explicit archive-extracted consumer pass for both linkages that fails
#     loudly instead of relying on a pipeline's last-command status,
#  2. what a downstream that pins C++17 actually gets from the installed
#     package (CMake raising the target standard versus a configure error),
#  3. how a shared-linkage consumer finds liblance_c.so when RPATH is skipped,
#  4. a sample of the build-path strings that survive in the released binaries.
#
# Usage: SOURCE=<product clone> EVID_ROOT=<evidence dir> bash probe.sh
set -euo pipefail
export LC_ALL=C

EVID=${EVID_ROOT:?set EVID_ROOT}
SOURCE=${SOURCE:?set SOURCE}
OUT=$EVID/supplement
WORK=$EVID/work
EX=$WORK/extracted
LOG=$OUT/logs
mkdir -p "$LOG"

log() { printf '%s\n' "$*" | tee -a "$LOG/probe.log"; }
die() { log "FAIL: $*"; exit 1; }
step() {
    local name=$1
    shift
    log "==> $name"
    "$@" 2>&1 | tee "$LOG/$name.log"
    local rc=${PIPESTATUS[0]}
    [ "$rc" -eq 0 ] || die "$name exited $rc"
}

[ -d "$EX/lib" ] || die "extracted payload missing at $EX"
[ -d "$SOURCE/examples/cmake-consumer" ] || die "consumer source missing"

check_consumers() {
    local prefix=$1 variant=$2
    local linkage build_dir out
    for linkage in static shared; do
        build_dir=$OUT/build/$variant-$linkage
        rm -rf "$build_dir"
        cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
            -DCMAKE_PREFIX_PATH="$prefix" \
            -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
            -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
            -DCMAKE_BUILD_TYPE=Release \
            -DLanceC_LINK="$linkage"
        cmake --build "$build_dir" --parallel 2

        grep -Fq "LanceC_DIR:PATH=$prefix/lib/cmake/LanceC" "$build_dir/CMakeCache.txt" \
            || die "$variant/$linkage resolved a different LanceC package"

        local link_txt=$build_dir/CMakeFiles/consumer.dir/link.txt
        if [ "$linkage" = static ]; then
            grep -Fq "$prefix/lib/liblance_c.a" "$link_txt" \
                || die "$variant/static did not link the extracted archive"
        else
            grep -Fq "$prefix/lib/liblance_c.so" "$link_txt" \
                || die "$variant/shared did not link the extracted shared library"
        fi
        grep -Fq "stage-retired" "$link_txt" \
            && die "$variant/$linkage link line references the retired prefix"

        set +e
        "$build_dir/consumer" >/dev/null 2>&1
        local rc=$?
        set -e
        [ "$rc" -eq 2 ] || die "$variant/$linkage usage exit $rc, expected 2"

        out=$("$build_dir/consumer" "file://$SOURCE/test_data/v0.27.1/pq_in_schema")
        [ "$out" = "rows: 512, version: 2" ] \
            || die "$variant/$linkage dataset read returned: $out"
        printf '%s/%s dataset read: %s\n' "$variant" "$linkage" "$out"

        rm -rf "$OUT/acceptance-$variant-$linkage.lance"
        "$build_dir/acceptance" "file://$OUT/acceptance-$variant-$linkage.lance" \
            | tee "$LOG/acceptance-$variant-$linkage.log"
        grep -q '^acceptance: PASS$' "$LOG/acceptance-$variant-$linkage.log" \
            || die "$variant/$linkage acceptance did not report PASS"

        printf 'max_glibc_%s_%s=%s\n' "$variant" "$linkage" \
            "$(readelf --dyn-syms --wide "$build_dir/consumer" |
                grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1)"

        if [ "$linkage" = shared ]; then
            printf 'runpath_%s=%s\n' "$variant" \
                "$(readelf -d "$build_dir/consumer" |
                    sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"
        fi
    done
}

# 1. Both linkages against the archive-extracted prefix, with the default
#    CMake build-tree RPATH the shipped consumer gate relies on.
step consumers-default check_consumers "$EX" default

# 2. A shared consumer built with CMAKE_SKIP_RPATH=ON: it must fail to start
#    without LD_LIBRARY_PATH and read the dataset with it.
skip_rpath_shared() {
    local build_dir=$OUT/build/skip-rpath-shared
    rm -rf "$build_dir"
    cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
        -DCMAKE_PREFIX_PATH="$EX" \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SKIP_RPATH=ON \
        -DLanceC_LINK=shared
    cmake --build "$build_dir" --parallel 2
    log "runpath_with_skip_rpath=$(readelf -d "$build_dir/consumer" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')"

    set +e
    "$build_dir/consumer" "file://$SOURCE/test_data/v0.27.1/pq_in_schema" \
        >"$LOG/skip-rpath-noldpath.log" 2>&1
    local rc=$?
    set -e
    log "skip_rpath_without_ld_library_path_exit=$rc"
    [ "$rc" -ne 0 ] || die "shared consumer started without a library search path"

    LD_LIBRARY_PATH=$EX/lib "$build_dir/consumer" \
        "file://$SOURCE/test_data/v0.27.1/pq_in_schema" \
        | tee "$LOG/skip-rpath-ldpath.log"
    grep -q '^rows: 512, version: 2$' "$LOG/skip-rpath-ldpath.log" \
        || die "shared consumer did not read the dataset with LD_LIBRARY_PATH"
}
step skip-rpath-shared skip_rpath_shared

# 3. A downstream that pins C++17: record the standard CMake actually compiles
#    with, and whether configuring fails.
cxx17_consumer() {
    local build_dir=$OUT/build/cxx17-consumer
    rm -rf "$build_dir"
    cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
        -DCMAKE_PREFIX_PATH="$EX" \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DLanceC_LINK=static
    cmake --build "$build_dir" --parallel 2
    log "cmake_cxx_standard_cache=$(sed -n 's/^CMAKE_CXX_STANDARD:.*=//p' "$build_dir/CMakeCache.txt")"
    log "consumer_compile_flags=$(grep -oE '\-std=[a-z+0-9]+' "$build_dir/CMakeFiles/consumer.dir/flags.make" | head -1)"
    log "cxx17_pinned_consumer_build=success"
}
step cxx17-consumer cxx17_consumer

# 4. Which build-path strings stay inside the released binaries.
path_strings() {
    local lib=$1 label=$2
    local sample hits
    sample=$(grep -a -o -F '/work/' "$lib" | wc -l)
    printf '%s_slash_work_occurrences=%s\n' "$label" "$sample"
    strings -a "$lib" | grep -F '/work/' | sed 's/^[[:space:]]*//' | sort -u |
        head -5 | sed "s/^/${label}_sample=/" | tee -a "$LOG/path-strings.log" || true
    hits=$(strings -a "$lib" | grep -F '/work/' |
        grep -cE '/work/(stage|stage-retired|extracted)(/|$)' || true)
    printf '%s_install_prefix_hits=%s\n' "$label" "${hits:-0}" | tee -a "$LOG/path-strings.log"
}
path_strings "$EX/lib/liblance_c.so" shared
path_strings "$EX/lib/liblance_c.a" static

cat >"$OUT/probe.env" <<EOF
probe=supplementary
product_head=$(cd "$SOURCE" && git rev-parse HEAD)
prefix=$EX
shared_sha256=$(sha256sum "$EX/lib/liblance_c.so" | awk '{print $1}')
static_sha256=$(sha256sum "$EX/lib/liblance_c.a" | awk '{print $1}')
result=pass
EOF
log "PROBE PASS"
