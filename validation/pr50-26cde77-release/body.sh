#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Release-path acceptance body for lance-c PR #50 head
# 26cde77e8c1acc538ec1a2f3be26f8eeea74373a.
#
# Runs INSIDE the digest-pinned manylinux_2_28_x86_64 container and reproduces
# .github/workflows/release.yml step for step: baseline env load, protoc action,
# clang install, configure with -DCMAKE_INSTALL_LIBDIR=lib, build, install,
# strip, export/staging verification, installed-prefix consumer, tar with both
# checksum branches, archive extraction, and extracted-prefix consumers.
#
# /source is mounted read-only. All writes go to $WORK and $EVID.

set -uo pipefail

SOURCE=${SOURCE:-/source}
EVID=${EVID:-/evidence}
WORK=${WORK:-/work}
TARGET_TRIPLE=${TARGET_TRIPLE:-x86_64-unknown-linux-gnu}
PRODUCT_VERSION=${PRODUCT_VERSION:-0.1.9}
ARCHIVE="lance-c-v${PRODUCT_VERSION}-${TARGET_TRIPLE}.tar.xz"
LOG_DIR=$EVID/logs
STATUS=$EVID/commands-status.txt
STAGE=$WORK/stage
EXTRACTED=$WORK/extracted

mkdir -p "$LOG_DIR" "$WORK"
: >"$STATUS"
FAILED_PHASES=()

phase() {
    local name=$1
    shift
    local start rc end
    start=$(date +%s)
    printf '[%s] START %-30s :: %s\n' "$(date -u +%FT%TZ)" "$name" "$*" >>"$STATUS"
    "$@" >"$LOG_DIR/$name.log" 2>&1
    rc=$?
    end=$(date +%s)
    printf '[%s] END   %-30s rc=%s wall=%ss\n' "$(date -u +%FT%TZ)" "$name" "$rc" "$((end - start))" >>"$STATUS"
    printf 'phase %-30s rc=%s wall=%ss\n' "$name" "$rc" "$((end - start))"
    if [ "$rc" -ne 0 ]; then
        FAILED_PHASES+=("$name")
    fi
    return "$rc"
}

fail_fast() {
    local rc=$1 name=$2
    if [ "$rc" -ne 0 ]; then
        printf 'FATAL: required phase %s failed (rc=%s), see %s/%s.log\n' \
            "$name" "$rc" "$LOG_DIR" "$name" >&2
        exit 1
    fi
}

note() {
    printf '%s=%s\n' "$1" "$2" >>"$EVID/artifacts.env"
}

p_record_env() {
    {
        printf 'container_image_ref=%s\n' "${CONTAINER_IMAGE_REF:-unknown}"
        printf 'container_image_id=%s\n' "${CONTAINER_IMAGE_ID:-unknown}"
        printf 'uname=%s\n' "$(uname -a)"
        # shellcheck disable=SC1091
        . /etc/os-release 2>/dev/null && printf 'os_release=%s\n' "${PRETTY_NAME:-unknown}"
        printf 'glibc=%s\n' "$(ldd --version 2>&1 | head -1)"
        printf 'cmake_preexisting_version=%s\n' "$(cmake --version 2>/dev/null | head -1 || echo MISSING)"
        printf 'cc_preexisting=%s\n' "$(command -v clang 2>/dev/null || echo MISSING)"
        local tool
        for tool in clang clang++ cmake ninja make git unzip curl xz tar strip readelf nm objdump ar \
            sha256sum sha512sum shasum perl python3 pkg-config file patchelf; do
            printf 'tool_%s=%s\n' "$tool" "$(command -v "$tool" 2>/dev/null || echo MISSING)"
        done
        if [ -x /usr/local/bin/manylinux-install-clang ]; then
            printf 'manylinux_install_clang=present\n'
        else
            printf 'manylinux_install_clang=MISSING\n'
        fi
        printf 'uid=%s\n' "$(id -u)"
    } >"$EVID/baseline-container.env"
}

# Workflow step "Load baseline floors".
p_baseline_env() {
    set -a
    # shellcheck disable=SC1091
    . "$SOURCE/.github/baseline.env"
    set +a
    printf 'baseline_env_MACOSX_DEPLOYMENT_TARGET=%s\n' "$MACOSX_DEPLOYMENT_TARGET" \
        >>"$EVID/baseline-container.env"
}

p_protoc() {
    cd "$SOURCE" || return 1
    PROTOC_VERSION=25.3 bash "$SOURCE/.github/actions/setup-protoc/install.sh"
    protoc --version
    sha256sum /usr/local/bin/protoc
    ls /usr/local/include/google/protobuf >/dev/null
}

p_clang() {
    manylinux-install-clang -v "${CLANG_VERSION:?CLANG_VERSION must be set}" || return 1
    clang --version
    clang++ --version
}

p_configure() {
    cmake -S "$SOURCE" -B "$WORK/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_LIBDIR=lib
    grep -E '^(CMAKE_INSTALL_LIBDIR|CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_C_COMPILER|LANCE_C_BUILD_TESTS):' \
        "$WORK/build/CMakeCache.txt" || true
    rustc --version --verbose
    cargo --version
    printf 'RUSTUP_TOOLCHAIN=%s CARGO_BUILD_JOBS=%s CC=%s CXX=%s\n' \
        "${RUSTUP_TOOLCHAIN:-unset}" "${CARGO_BUILD_JOBS:-unset}" "${CC:-unset}" "${CXX:-unset}"
}

p_build() {
    cmake --build "$WORK/build" --parallel
    find "$WORK/build" -name 'liblance_c.so' -o -name 'liblance_c.a' | sort
    find "$WORK/build" -maxdepth 4 -type d -name 'release' -o -maxdepth 4 -type d -name 'debug' | sort
    tail -5 "$LOG_DIR/p_build.log"
}

p_install() {
    cmake --install "$WORK/build" --prefix "$STAGE"
    find "$STAGE" -maxdepth 3 | sort
}

p_strip() {
    sha256sum "$STAGE/lib/liblance_c.so"
    wc -c <"$STAGE/lib/liblance_c.so"
    bash "$SOURCE/.github/scripts/strip-release-shared.sh" "$STAGE" Linux
    sha256sum "$STAGE/lib/liblance_c.so"
    wc -c <"$STAGE/lib/liblance_c.so"
    sha256sum "$STAGE/lib/liblance_c.a"
}

p_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$STAGE/lib/liblance_c.so" "$STAGE/include/lance/lance.h" linux
}

p_staging() {
    bash "$SOURCE/.github/scripts/verify-linux-staging.sh" "$STAGE" "$TARGET_TRIPLE"
}

p_consumer_stage() {
    bash "$SOURCE/.github/scripts/verify-cmake-consumer.sh" --from-install-prefix "$STAGE"
}

# Workflow "Tar" step, verbatim, with XZ_OPT from the job env.
p_tar() {
    cd "$WORK" || return 1
    XZ_OPT=-T0 tar -C stage -cJf "$ARCHIVE" .
    xz --version
    wc -c <"$ARCHIVE"
    sha256sum "$ARCHIVE"
    tar -tf "$ARCHIVE" | wc -l
}

# Workflow checksum block: primary branch as written in release.yml.
p_checksums_primary() {
    cd "$WORK" || return 1
    if command -v shasum >/dev/null 2>&1; then
        printf 'branch=shasum\n'
        shasum -a 256 "$ARCHIVE" >"$ARCHIVE.sha256"
        shasum -a 512 "$ARCHIVE" >"$ARCHIVE.sha512"
    else
        printf 'branch=sha256sum-fallback\n'
        sha256sum "$ARCHIVE" >"$ARCHIVE.sha256"
        sha512sum "$ARCHIVE" >"$ARCHIVE.sha512"
    fi
    cat "$ARCHIVE.sha256" "$ARCHIVE.sha512"
}

# Force the else-branch of the same block so both branches are exercised on this
# image, then require identical digests.
p_checksums_forced() {
    local forced_dir=$WORK/no-shasum
    rm -rf "$forced_dir"
    mkdir -p "$forced_dir"
    ln -s "$(command -v sha256sum)" "$forced_dir/sha256sum"
    ln -s "$(command -v sha512sum)" "$forced_dir/sha512sum"
    cd "$WORK" || return 1
    env -i PATH="$forced_dir" bash -c '
        set -eu
        if command -v shasum >/dev/null 2>&1; then
            echo "unexpected shasum on restricted PATH" >&2
            exit 1
        fi
        sha256sum "$1" >"$1.forced.sha256"
        sha512sum "$1" >"$1.forced.sha512"
    ' bash "$ARCHIVE"
    diff <(awk '{print $1}' "$ARCHIVE.sha256") <(awk '{print $1}' "$ARCHIVE.forced.sha256")
    diff <(awk '{print $1}' "$ARCHIVE.sha512") <(awk '{print $1}' "$ARCHIVE.forced.sha512")
    awk '{print $1}' "$ARCHIVE.forced.sha256" "$ARCHIVE.forced.sha512"
}

p_extract() {
    rm -rf "$EXTRACTED"
    mkdir -p "$EXTRACTED"
    tar -C "$EXTRACTED" -xf "$WORK/$ARCHIVE"
    tar -tf "$WORK/$ARCHIVE" | awk -F/ 'NF>1 {print $2}' | sort -u
    tar -tf "$WORK/$ARCHIVE" | grep -E '^/|(^|/)\.\.(/|$)' && {
        echo "archive contains absolute or parent-relative members" >&2
        return 1
    }
    find "$EXTRACTED" -maxdepth 3 | sort
}

# Retire the staged prefix so nothing can resolve against it any more.
p_retire_stage() {
    mv "$STAGE" "$WORK/stage-retired"
    find "$WORK/stage-retired" -maxdepth 1 | sort
}

p_consumer_extracted_official() {
    bash "$SOURCE/.github/scripts/verify-cmake-consumer.sh" --from-install-prefix "$EXTRACTED"
}

# Explicit extracted-prefix consumers with rpath disabled, so the recorded link
# line and ldd output prove which physical prefix was used.
p_consumer_extracted_explicit() {
    local linkage build_dir link_txt acceptance_exe
    for linkage in static shared; do
        build_dir=$WORK/consumers/extracted-$linkage
        rm -rf "$build_dir"
        mkdir -p "$build_dir"
        cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
            -DCMAKE_PREFIX_PATH="$EXTRACTED" \
            -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
            -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_SKIP_RPATH=ON \
            -DLanceC_LINK="$linkage"
        cmake --build "$build_dir" --parallel 2
        grep -Fq "LanceC_DIR:PATH=$EXTRACTED/lib/cmake/LanceC" "$build_dir/CMakeCache.txt"
        link_txt=$build_dir/CMakeFiles/consumer.dir/link.txt
        acceptance_exe=$build_dir/CMakeFiles/acceptance.dir/link.txt
        grep -Fq "$WORK/stage-retired" "$link_txt" "$acceptance_exe" && {
            echo "$linkage consumer link line references the retired prefix" >&2
            return 1
        }
        if [ "$linkage" = static ]; then
            grep -Fq "$EXTRACTED/lib/liblance_c.a" "$link_txt"
        else
            grep -Fq "$EXTRACTED/lib/liblance_c.so" "$link_txt"
        fi
        # usage path must exit 2
        local rc=0
        "$build_dir/consumer" >/dev/null 2>&1 || rc=$?
        printf '%s consumer usage exit=%s\n' "$linkage" "$rc"
        [ "$rc" -eq 2 ]
        "$build_dir/consumer" "file://$SOURCE/test_data/v0.27.1/pq_in_schema"
        "$build_dir/acceptance" "file://$WORK/datasets/acceptance-$linkage.lance"
        ldd "$build_dir/consumer" || true
        for exe in consumer acceptance; do
            printf 'max_glibc_%s_%s=%s\n' "$linkage" "$exe" \
                "$(readelf --dyn-syms --wide "$build_dir/$exe" |
                    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1)"
        done
    done
}

p_audit_layout() {
    printf '== symlinks in extracted\n'
    find "$EXTRACTED" -type l | wc -l
    printf '== lib64 in extracted\n'
    if [ -e "$EXTRACTED/lib64" ]; then echo present; else echo absent; fi
    printf '== extracted dirs\n'
    find "$EXTRACTED" -maxdepth 2 -type d | sort
    printf '== installed cmake package files\n'
    find "$EXTRACTED/lib/cmake" -type f | sort
    printf '== installed include files\n'
    find "$EXTRACTED/include" -type f | sort
    printf '== library sizes\n'
    wc -c "$EXTRACTED/lib/liblance_c.a" "$EXTRACTED/lib/liblance_c.so"
    printf '== static archive members\n'
    ar t "$EXTRACTED/lib/liblance_c.a" | wc -l
    printf '== exported lance_* symbols in extracted shared lib\n'
    nm -D --defined-only "$EXTRACTED/lib/liblance_c.so" |
        awk '{print $3}' | grep -E '^lance_' | sort -u | wc -l
    printf '== header lance_* declarations\n'
    grep -oE '\blance_[a-z0-9_]+\(' "$EXTRACTED/include/lance/lance.h" | sort -u | wc -l
    printf '== archive member modes (top 5)\n'
    tar -tvf "$WORK/$ARCHIVE" | head -5
    printf '== sha256 of extracted payload\n'
    sha256sum "$EXTRACTED/lib/liblance_c.a" "$EXTRACTED/lib/liblance_c.so"
}

p_audit_elf() {
    printf '== dynamic section (staged copy is retired; inspect extracted)\n'
    readelf -d "$EXTRACTED/lib/liblance_c.so" |
        grep -E 'NEEDED|SONAME|RPATH|RUNPATH' || true
    printf '== max glibc version reference in shared lib\n'
    readelf --dyn-syms --wide "$EXTRACTED/lib/liblance_c.so" |
        grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1
    printf '== machine\n'
    readelf -h "$EXTRACTED/lib/liblance_c.so" | grep -E 'Class|Machine|Type'
}

p_audit_paths() {
    local hits
    for pattern in '/work/' '/source/' 'stage-retired'; do
        hits=$(grep -a -o -F "$pattern" "$EXTRACTED/lib/liblance_c.so" | wc -l)
        printf 'shared_path_hits%s=%s\n' "$pattern" "$hits"
        hits=$(grep -a -o -F "$pattern" "$EXTRACTED/lib/liblance_c.a" | wc -l)
        printf 'static_path_hits%s=%s\n' "$pattern" "$hits"
    done
}

p_cxx_gate() {
    printf '#include <lance/lance.hpp>\nint main() { return 0; }\n' >"$WORK/cxx_probe.cpp"
    local rc17=0 rc20=0 rc_cmake=0
    clang++ -std=c++17 -fsyntax-only -I "$EXTRACTED/include" "$WORK/cxx_probe.cpp" || rc17=$?
    clang++ -std=c++20 -fsyntax-only -I "$EXTRACTED/include" "$WORK/cxx_probe.cpp" || rc20=$?
    printf 'header_syntax_only_cxx17_rc=%s\n' "$rc17"
    printf 'header_syntax_only_cxx20_rc=%s\n' "$rc20"
    printf '== interface compile features exported by installed package\n'
    grep -rn 'cxx_std_' "$EXTRACTED/lib/cmake/LanceC" || true
    rm -rf "$WORK/cxx17-consumer"
    cmake -S "$SOURCE/examples/cmake-consumer" -B "$WORK/cxx17-consumer" \
        -DCMAKE_PREFIX_PATH="$EXTRACTED" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DLanceC_LINK=static || rc_cmake=$?
    printf 'cxx17_find_package_configure_rc=%s\n' "$rc_cmake"
    grep -m3 -E 'CXX_STANDARD|cxx_std_20|requires the language dialect' \
        "$LOG_DIR/p_cxx_gate.log" || true
}

p_identities() {
    cd "$SOURCE" || return 1
    printf '== product source identity\n'
    git rev-parse HEAD 2>/dev/null || echo "no-git-metadata"
    printf '== product file hashes\n'
    sha256sum include/lance/lance.h Cargo.lock Cargo.toml
    printf '== extracted payload hashes\n'
    sha256sum "$EXTRACTED/lib/liblance_c.a" "$EXTRACTED/lib/liblance_c.so"
    printf '== archive hashes\n'
    sha256sum "$WORK/$ARCHIVE"
}

p_summary() {
    local shared_max_glibc static_max_glibc
    shared_max_glibc=$(readelf --dyn-syms --wide "$EXTRACTED/lib/liblance_c.so" |
        grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1)
    static_max_glibc=$(readelf --dyn-syms --wide "$WORK/consumers/extracted-static/consumer" 2>/dev/null |
        grep -oE 'GLIBC_[0-9]+(\.[0-9]+)*' | sort -uV | tail -1)
    {
        printf 'result=%s\n' "$([ "${#FAILED_PHASES[@]}" -eq 0 ] && echo pass || echo fail)"
        printf 'failed_phases=%s\n' "${FAILED_PHASES[*]:-none}"
        printf 'product_head=%s\n' "$(cd "$SOURCE" && git rev-parse HEAD 2>/dev/null || echo unknown)"
        printf 'target=%s\n' "$TARGET_TRIPLE"
        printf 'product_version=%s\n' "$PRODUCT_VERSION"
        printf 'image_ref=%s\n' "${CONTAINER_IMAGE_REF:-unknown}"
        printf 'image_id=%s\n' "${CONTAINER_IMAGE_ID:-unknown}"
        printf 'glibc=%s\n' "$(ldd --version | head -1 | awk '{print $NF}')"
        printf 'cmake_version=%s\n' "$(cmake --version | head -1)"
        printf 'clang_version=%s\n' "$(clang --version | head -1)"
        printf 'protoc_version=%s\n' "$(protoc --version)"
        printf 'rustc_version=%s\n' "$(rustc --version)"
        printf 'shared_max_glibc=%s\n' "$shared_max_glibc"
        printf 'static_consumer_max_glibc=%s\n' "$static_max_glibc"
        printf 'installed_libdir_lib_file=%s\n' "$([ -f "$EXTRACTED/lib/liblance_c.so" ] && echo yes || echo no)"
        printf 'installed_libdir_lib64_exists=%s\n' "$([ -e "$EXTRACTED/lib64" ] && echo yes || echo no)"
        printf 'extracted_symlink_count=%s\n' "$(find "$EXTRACTED" -type l | wc -l)"
        printf 'export_count=%s\n' "$(nm -D --defined-only "$EXTRACTED/lib/liblance_c.so" |
            awk '{print $3}' | grep -E '^lance_' | sort -u | wc -l)"
        printf 'archive=%s\n' "$WORK/$ARCHIVE"
        printf 'archive_sha256=%s\n' "$(sha256sum "$WORK/$ARCHIVE" | awk '{print $1}')"
        printf 'archive_sha512=%s\n' "$(sha512sum "$WORK/$ARCHIVE" | awk '{print $1}')"
        printf 'archive_bytes=%s\n' "$(wc -c <"$WORK/$ARCHIVE")"
        printf 'archive_members=%s\n' "$(tar -tf "$WORK/$ARCHIVE" | wc -l)"
        printf 'static_sha256=%s\n' "$(sha256sum "$EXTRACTED/lib/liblance_c.a" | awk '{print $1}')"
        printf 'shared_stripped_sha256=%s\n' "$(sha256sum "$EXTRACTED/lib/liblance_c.so" | awk '{print $1}')"
        printf 'static_bytes=%s\n' "$(wc -c <"$EXTRACTED/lib/liblance_c.a")"
        printf 'shared_bytes=%s\n' "$(wc -c <"$EXTRACTED/lib/liblance_c.so")"
        printf 'header_sha256=%s\n' "$(sha256sum "$EXTRACTED/include/lance/lance.h" | awk '{print $1}')"
    } >"$EVID/summary.env"
    cat "$EVID/summary.env"
}

p_prepare_work() {
    # Start from a clean work tree so recorded artifacts cannot be leftovers.
    local stale
    for stale in "$WORK/build" "$STAGE" "$WORK/stage-retired" "$WORK/consumers" \
        "$WORK/datasets" "$WORK/cxx17-consumer" "$WORK/no-shasum"; do
        if [ -e "$stale" ]; then
            printf 'removing stale %s\n' "$stale"
            rm -rf "$stale"
        fi
    done
    rm -f "$WORK/$ARCHIVE" "$WORK/$ARCHIVE.sha256" "$WORK/$ARCHIVE.sha512" \
        "$WORK/$ARCHIVE.forced.sha256" "$WORK/$ARCHIVE.forced.sha512"
    mkdir -p "$WORK/datasets"
    ls -la "$WORK"
}

phase p_prepare_work p_prepare_work
fail_fast $? p_prepare_work

phase p_record_env p_record_env
phase p_baseline_env p_baseline_env
phase p_protoc p_protoc
fail_fast $? p_protoc
phase p_clang p_clang
fail_fast $? p_clang
phase p_configure p_configure
fail_fast $? p_configure
phase p_build p_build
fail_fast $? p_build
phase p_install p_install
fail_fast $? p_install
phase p_strip p_strip
fail_fast $? p_strip
phase p_exports p_exports
fail_fast $? p_exports
phase p_staging p_staging
fail_fast $? p_staging
phase p_consumer_stage p_consumer_stage
fail_fast $? p_consumer_stage
phase p_tar p_tar
fail_fast $? p_tar
phase p_checksums_primary p_checksums_primary
fail_fast $? p_checksums_primary
phase p_checksums_forced p_checksums_forced
fail_fast $? p_checksums_forced
phase p_extract p_extract
fail_fast $? p_extract
phase p_retire_stage p_retire_stage
fail_fast $? p_retire_stage
phase p_consumer_extracted_official p_consumer_extracted_official
fail_fast $? p_consumer_extracted_official
phase p_consumer_extracted_explicit p_consumer_extracted_explicit
fail_fast $? p_consumer_extracted_explicit
phase p_audit_layout p_audit_layout
phase p_audit_elf p_audit_elf
phase p_audit_paths p_audit_paths
# C++17 gate probe is expected to make the configure step fail; do not gate on it.
phase p_cxx_gate p_cxx_gate
phase p_identities p_identities
phase p_summary p_summary

if [ "${#FAILED_PHASES[@]}" -ne 0 ]; then
    printf 'BODY RESULT: FAIL (%s)\n' "${FAILED_PHASES[*]}"
    exit 1
fi
printf 'BODY RESULT: PASS\n'
