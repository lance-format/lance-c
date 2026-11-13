#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Issue #103 acceptance driver: install with an explicit lib directory and run
# installed and extracted-archive consumers against that physical layout.
#
# This script changes no product files. The committed source tree is expected to
# be the product commit plus this validation directory only.

set -euo pipefail
export LC_ALL=C

SCRIPT_PATH=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
MODE=outer
SOURCE=
ROOT=
EVIDENCE=
WORK=
PRODUCT_COMMIT=
VALIDATION_COMMIT=
RESUME=0
IMAGE=quay.io/pypa/manylinux_2_28_x86_64
IMAGE_DIGEST=sha256:9555ba6c479fa69a2aa80ad1dc57154a60d562b19df159361d9379a02beb1e36
CLANG_VERSION=v20.1.8.0
PROTOC_VERSION=25.3
PROTOC_SHA256=f853e691868d0557425ea290bf7ba6384eef2fa9b04c323afab49a770ba9da80
TARGET=x86_64-unknown-linux-gnu
VALIDATION_DIR=validation/manylinux103-lib-acceptance
CONTAINER_NAME=
STATUS_FILE=
LOG_DIR=
resume_args=()

usage() {
    cat <<'EOF'
usage:
  run.sh --source <git-root> --root <evidence-root> --product-commit <sha>
  run.sh --container-setup --evidence <dir>
  run.sh --container-body --source <dir> --work <dir> --evidence <dir> \
    --product-commit <sha> --validation-commit <sha> [--resume]
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --container-setup) MODE=container-setup; shift ;;
        --container-body) MODE=container-body; shift ;;
        --source) SOURCE=$2; shift 2 ;;
        --root) ROOT=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --evidence) EVIDENCE=$2; shift 2 ;;
        --product-commit) PRODUCT_COMMIT=$2; shift 2 ;;
        --validation-commit) VALIDATION_COMMIT=$2; shift 2 ;;
        --resume) RESUME=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

assert_eq() {
    local actual=$1 expected=$2 message=$3
    [ "$actual" = "$expected" ] ||
        fail "$message: expected '$expected', found '$actual'"
}

assert_nonempty() {
    [ -n "$1" ] || fail "$2"
}

assert_file() {
    [ -f "$1" ] || fail "missing file: $1"
}

assert_dir() {
    [ -d "$1" ] || fail "missing directory: $1"
}

version_le() {
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]
}

append_status() {
    local state=$1
    shift
    local tmp
    tmp=$(mktemp "${STATUS_FILE}.XXXXXX")
    {
        cat "$STATUS_FILE"
        printf '%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            "$state" "$*"
    } >"$tmp"
    mv -f "$tmp" "$STATUS_FILE"
}

run_step() {
    local name=$1
    shift
    local log="$LOG_DIR/$name.log"
    local rc

    append_status START "$name"
    printf '$ %s\n' "$*" >"$log"
    set +e
    ( set -euo pipefail; "$@" ) >>"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        append_status FAIL "$name rc=$rc log=$log"
        return "$rc"
    fi
    append_status PASS "$name log=$log"
}

manifest_tree() {
    local root=$1
    local out=$2
    (
        cd "$root"
        find . -type f -print0 | sort -z | xargs -0 sha256sum
    ) >"$out"
}

container_setup() {
    local tools=/opt/clang/bin
    local zip=/tmp/protoc-${PROTOC_VERSION}-linux-x86_64.zip
    local release_json=/tmp/protoc-release-${PROTOC_VERSION}.json
    local vendor_checksum=unknown

    [ "$(id -u)" = 0 ] || fail "container setup must run as root"
    mkdir -p "$EVIDENCE" /work /home/xuanwo/.cache /home/xuanwo/.config
    chown 1000:1000 /home/xuanwo /home/xuanwo/.cache /home/xuanwo/.config

    {
        echo "setup_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "image=$IMAGE@$IMAGE_DIGEST"
        echo "base_glibc=$(getconf GNU_LIBC_VERSION)"
        echo "base_os_begin"
        cat /etc/os-release
        echo "base_os_end"
        echo "clang_installer=$(command -v manylinux-install-clang)"
        echo "clang_version=$CLANG_VERSION"
        echo "protoc_version=$PROTOC_VERSION"
        echo "protoc_sha256_expected=$PROTOC_SHA256"
    } >"$EVIDENCE/setup-identity.txt"

    manylinux-install-clang -v "$CLANG_VERSION"
    [ -x "$tools/clang" ] || fail "Clang was not installed at $tools/clang"
    [ -x "$tools/clang++" ] || fail "Clang++ was not installed at $tools/clang++"

    # Check whether the upstream release publishes an authenticated checksum
    # next to the binary. Record the result either way. The pinned digest above
    # is a local pin, not an upstream-attested checksum.
    if curl -fsSL --retry 2 \
        "https://api.github.com/repos/protocolbuffers/protobuf/releases/tags/v${PROTOC_VERSION}" \
        -o "$release_json"; then
        if grep -Eq "\"name\":[[:space:]]*\"protoc-${PROTOC_VERSION}-linux-x86_64\\.zip\\.(sha256|sha512|asc)\"" \
            "$release_json"; then
            vendor_checksum=present
        else
            vendor_checksum=absent
        fi
    else
        vendor_checksum=lookup-failed
    fi
    {
        echo "protoc_vendor_checksum=$vendor_checksum"
        echo "protoc_authenticity_limit=no-upstream-checksum-asset; local-pinned-sha256-only"
    } >>"$EVIDENCE/setup-identity.txt"

    curl -fsSL --retry 3 -o "$zip" \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-x86_64.zip"
    echo "$PROTOC_SHA256  $zip" | sha256sum -c -
    unzip -q -o "$zip" -d /usr/local
    rm -f "$zip" "$release_json"

    {
        echo "clang=$($tools/clang --version | awk 'NR == 1')"
        echo "clangxx=$($tools/clang++ --version | awk 'NR == 1')"
        echo "protoc=$(protoc --version)"
        echo "protoc_path=$(command -v protoc)"
    } >>"$EVIDENCE/setup-identity.txt"

    chown -R 1000:1000 /work "$EVIDENCE"
}

record_environment() {
    local name
    mkdir -p "$EVIDENCE/logs"

    {
        echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "hostname=$(uname -n)"
        echo "arch=$(uname -m)"
        echo "uname=$(uname -a)"
        echo "nproc=$(nproc)"
        echo "id=$(id)"
        echo "os_release_begin"
        cat /etc/os-release
        echo "os_release_end"
        echo "almalinux_release=$(cat /etc/almalinux-release 2>/dev/null || true)"
        echo "libc=$(getconf GNU_LIBC_VERSION)"
        echo "ldd_begin"
        ldd --version | sed -n '1,2p'
        echo "ldd_end"
        echo "memory_begin"
        free -h
        echo "memory_end"
        echo "disk_begin"
        df -h /work /source
        echo "disk_end"
        echo "source_owner=$(stat -c '%u:%g' /source)"
        echo "work_owner=$(stat -c '%u:%g' /work)"
        echo "allowlisted_environment_begin"
        for name in \
            HOME PATH CC CXX CARGO_HOME RUSTUP_HOME CARGO_BUILD_JOBS \
            RUST_TEST_THREADS CMAKE_BUILD_PARALLEL_LEVEL RUSTUP_TOOLCHAIN \
            LD_LIBRARY_PATH; do
            if [ -n "${!name+x}" ]; then
                printf '%s=%s\n' "$name" "${!name}"
            else
                printf '%s=<unset>\n' "$name"
            fi
        done
        echo "allowlisted_environment_end"
        echo "cargo_profile_release_lto=${CARGO_PROFILE_RELEASE_LTO-<unset>}"
        echo "rustflags=${RUSTFLAGS-<unset>}"
        echo "cc=${CC-}"
        echo "cxx=${CXX-}"
        echo "rustc_path=$(command -v rustc)"
        echo "cargo_path=$(command -v cargo)"
        rustc -vV
        echo "cargo=$(cargo --version)"
        echo "clang=$($CC --version | awk 'NR == 1')"
        echo "clangxx=$($CXX --version | awk 'NR == 1')"
        echo "gcc=$(gcc --version | awk 'NR == 1')"
        echo "gxx=$(g++ --version | awk 'NR == 1')"
        echo "cmake=$(cmake --version | awk 'NR == 1')"
        echo "make=$(make --version | awk 'NR == 1')"
        echo "protoc=$(protoc --version)"
        echo "git=$(git --version)"
        echo "python=$(python3 --version)"
        echo "readelf=$(readelf --version | awk 'NR == 1')"
        echo "nm=$(nm --version | awk 'NR == 1')"
        echo "strip=$(strip --version | awk 'NR == 1')"
        echo "ar=$(ar --version | awk 'NR == 1')"
        echo "tar=$(tar --version | awk 'NR == 1')"
        echo "xz=$(xz --version | awk 'NR == 1')"
        echo "pkg_config=$(pkg-config --version)"
        echo "mbx=$(mbx --version 2>&1 | awk 'NR == 1')"
    } >"$EVIDENCE/environment.txt"

    mbx doctor >"$EVIDENCE/logs/mbx-doctor.log" 2>&1 || true
    mbx settings ls >"$EVIDENCE/logs/mbx-settings.log" 2>&1 || true
}

record_source_identity() {
    local head changed dirty pin_count product_diff
    local product_paths=(
        src include Cargo.toml Cargo.lock build.rs CMakeLists.txt cmake
        .github/scripts examples/cmake-consumer test_data .github/workflows
    )

    head=$(git -C "$SOURCE" rev-parse HEAD)
    assert_eq "$head" "$VALIDATION_COMMIT" "source HEAD"
    assert_eq "$(git -C "$SOURCE" rev-parse "$head^")" "$PRODUCT_COMMIT" \
        "validation commit parent"
    git -C "$SOURCE" merge-base --is-ancestor "$PRODUCT_COMMIT" "$head" ||
        fail "product commit is not an ancestor of validation HEAD"

    changed=$(git -C "$SOURCE" diff --name-only "$PRODUCT_COMMIT" "$head")
    if [ -n "$changed" ] &&
        printf '%s\n' "$changed" |
            grep -Ev "^${VALIDATION_DIR}/" >/dev/null; then
        fail "validation branch changes product files"
    fi
    product_diff=$(git -C "$SOURCE" diff --name-only \
        "$PRODUCT_COMMIT" "$head" -- "${product_paths[@]}")
    assert_eq "$product_diff" "" "product path diff"

    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source worktree cleanliness"

    pin_count=$(grep -c '40f15ed1b71b66950961b175829fdf17a763f108' \
        "$SOURCE/Cargo.lock" || true)
    assert_eq "$pin_count" "23" "Lance pin reference count"
    grep -q '^rust-version = "1.94.0"$' "$SOURCE/Cargo.toml" ||
        fail "Cargo.toml does not declare Rust 1.94.0"

    {
        echo "product_commit=$PRODUCT_COMMIT"
        echo "product_tree=$(git -C "$SOURCE" rev-parse "$PRODUCT_COMMIT^{tree}")"
        echo "product_subject=$(git -C "$SOURCE" log -1 --pretty=%s "$PRODUCT_COMMIT")"
        echo "validation_commit=$head"
        echo "validation_tree=$(git -C "$SOURCE" rev-parse "$head^{tree}")"
        echo "validation_subject=$(git -C "$SOURCE" log -1 --pretty=%s "$head")"
        echo "validation_parent=$(git -C "$SOURCE" rev-parse "$head^")"
        echo "validation_diff_paths_begin"
        printf '%s\n' "$changed"
        echo "validation_diff_paths_end"
        echo "product_path_diff=$product_diff"
        echo "source_status_porcelain=$dirty"
        echo "lance_pin_count=$pin_count"
        echo "validation_script_sha256=$(sha256sum "$SCRIPT_PATH" | awk '{print $1}')"
        echo "product_tree_objects_begin"
        git -C "$SOURCE" ls-tree "$PRODUCT_COMMIT" -- \
            src include Cargo.toml Cargo.lock build.rs CMakeLists.txt cmake \
            .github/scripts examples/cmake-consumer test_data .github/workflows
        echo "product_tree_objects_end"
        echo "validation_tree_objects_begin"
        git -C "$SOURCE" ls-tree "$head" -- "$VALIDATION_DIR"
        echo "validation_tree_objects_end"
        echo "header_sha256=$(sha256sum "$SOURCE/include/lance/lance.h" |
            awk '{print $1}')"
        echo "lock_sha256=$(sha256sum "$SOURCE/Cargo.lock" | awk '{print $1}')"
        echo "manifest_sha256=$(sha256sum "$SOURCE/Cargo.toml" |
            awk '{print $1}')"
    } >"$EVIDENCE/source-identity.txt"
}

setup_status() {
    STATUS_FILE="$EVIDENCE/commands-status.txt"
    LOG_DIR="$EVIDENCE/logs"
    mkdir -p "$LOG_DIR"
    if [ -e "$STATUS_FILE" ]; then
        [ "$RESUME" = 1 ] ||
            fail "evidence directory already contains a run: $EVIDENCE"
        append_status RESUME "resume requested"
    else
        : >"$STATUS_FILE"
    fi
    trap 'rc=$?; append_status SCRIPT_EXIT "rc=$rc" 2>/dev/null || true; exit "$rc"' EXIT
}

configure_release() {
    cmake -S "$SOURCE" -B "$WORK/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

    local cache_libdir
    cache_libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' \
        "$WORK/build/CMakeCache.txt" | tail -1)
    assert_eq "$cache_libdir" "lib" "CMAKE_INSTALL_LIBDIR cache value"
    if grep -Eq '^CMAKE_INSTALL_(FULL_)?LIBDIR[^=]*=.*lib64' \
        "$WORK/build/CMakeCache.txt"; then
        fail "a CMake install libdir variable still resolves to lib64"
    fi
}

build_release() {
    cmake --build "$WORK/build" --parallel 1
}

install_release() {
    [ ! -e "$WORK/stage" ] || fail "install prefix already exists: $WORK/stage"
    cmake --install "$WORK/build" --prefix "$WORK/stage"
}

record_artifacts() {
    local release_so release_a
    release_so=$(find "$WORK/build" -type f \
        -path '*/release/liblance_c.so' -print -quit)
    release_a=$(find "$WORK/build" -type f \
        -path '*/release/liblance_c.a' -print -quit)
    assert_nonempty "$release_so" "release cdylib was not produced"
    assert_nonempty "$release_a" "release static library was not produced"
    {
        echo "release_so=$release_so"
        echo "release_a=$release_a"
        sha256sum "$release_so" "$release_a"
        stat -c '%n %s' "$release_so" "$release_a"
    } >"$EVIDENCE/build-artifacts.txt"
}

assert_standard_layout() {
    local prefix=$1
    local pc="$prefix/lib/pkgconfig/lance-c.pc"
    local cfg="$prefix/lib/cmake/LanceC/LanceCConfig.cmake"
    local symlinks

    assert_dir "$prefix/lib"
    [ ! -L "$prefix/lib" ] || fail "library directory is a symlink: $prefix/lib"
    [ ! -e "$prefix/lib64" ] || fail "unexpected lib64 exists: $prefix/lib64"
    symlinks=$(find "$prefix" -type l -print -quit)
    assert_eq "$symlinks" "" "symlinks in installed layout"

    for file in \
        "$prefix/lib/liblance_c.a" \
        "$prefix/lib/liblance_c.so" \
        "$cfg" \
        "$prefix/lib/cmake/LanceC/LanceCConfigVersion.cmake" \
        "$pc" \
        "$prefix/include/lance/lance.h" \
        "$prefix/include/lance/lance.hpp" \
        "$prefix/share/licenses/lance-c/LICENSE"; do
        assert_file "$file"
    done

    grep -q '^libdir=${prefix}/lib$' "$pc" ||
        fail "pkg-config libdir is not \${prefix}/lib"
    if grep -Eq 'lib64|/work/|/source/' "$pc"; then
        fail "pkg-config file contains a non-relocatable or lib64 path"
    fi
    grep -Eq '_LANCE_C_LIB_DIR[[:space:]]+"\$\{PACKAGE_PREFIX_DIR\}/lib"' "$cfg" ||
        fail "CMake config library directory is not \${PACKAGE_PREFIX_DIR}/lib"
    if grep -Eq 'lib64|/work/|/source/' "$cfg" ||
        grep -Eq 'lib64|/work/|/source/' \
            "$prefix/lib/cmake/LanceC/LanceCConfigVersion.cmake"; then
        fail "CMake package files contain a non-relocatable or lib64 path"
    fi
}

record_install_layout() {
    local prefix=$1
    local label=$2
    local out="$EVIDENCE/${label}-install-layout.txt"

    assert_standard_layout "$prefix"
    {
        echo "prefix=$prefix"
        echo "cmake_install_libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' \
            "$WORK/build/CMakeCache.txt" | tail -1)"
        echo "lib_is_symlink=$(test -L "$prefix/lib" && echo yes || echo no)"
        echo "lib64_exists=$(test -e "$prefix/lib64" && echo yes || echo no)"
        echo "symlink_count=$(find "$prefix" -type l | wc -l)"
        echo "file_count=$(find "$prefix" -type f | wc -l)"
        echo "du_begin"
        du -sh "$prefix"
        echo "du_end"
        echo "entries_begin"
        find "$prefix" -mindepth 1 -maxdepth 1 -printf '%y %f\n' | sort
        echo "entries_end"
        echo "files_begin"
        find "$prefix" -type f -printf '%P %s\n' | sort
        echo "files_end"
        echo "pkgconfig_begin"
        cat "$prefix/lib/pkgconfig/lance-c.pc"
        echo "pkgconfig_end"
        echo "cmake_config_paths_begin"
        grep -n 'PACKAGE_PREFIX_DIR\|_LANCE_C_LIB_DIR\|_LANCE_C_INCLUDE_DIR' \
            "$prefix/lib/cmake/LanceC/LanceCConfig.cmake"
        echo "cmake_config_paths_end"
    } >"$out"
    manifest_tree "$prefix" "$EVIDENCE/${label}-manifest.sha256"
    printf '%s\n' "$prefix" >"$EVIDENCE/${label}-prefix.txt"
}

record_stage_prestrip() {
    {
        sha256sum "$WORK/stage/lib/liblance_c.a" \
            "$WORK/stage/lib/liblance_c.so"
        stat -c '%n %s' "$WORK/stage/lib/liblance_c.a" \
            "$WORK/stage/lib/liblance_c.so"
    } >"$EVIDENCE/stage-prestrip.sha256"
}

strip_shared() {
    bash "$SOURCE/.github/scripts/strip-release-shared.sh" \
        "$WORK/stage" linux
}

record_stage_poststrip() {
    {
        sha256sum "$WORK/stage/lib/liblance_c.a" \
            "$WORK/stage/lib/liblance_c.so"
        stat -c '%n %s' "$WORK/stage/lib/liblance_c.a" \
            "$WORK/stage/lib/liblance_c.so"
    } >"$EVIDENCE/stage-poststrip.sha256"
    manifest_tree "$WORK/stage" "$EVIDENCE/stage-manifest.sha256"
}

verify_static_untouched() {
    local before after
    before=$(awk '$2 ~ /liblance_c\.a$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    after=$(sha256sum "$WORK/stage/lib/liblance_c.a" | awk '{print $1}')
    assert_eq "$after" "$before" "static archive changed during stripping"
}

verify_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$WORK/stage/lib/liblance_c.so" \
        "$WORK/stage/include/lance/lance.h" linux
}

verify_extracted_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$WORK/extracted/lib/liblance_c.so" \
        "$WORK/extracted/include/lance/lance.h" linux
}

record_export_sets() {
    local header="$WORK/stage/include/lance/lance.h"
    local library="$WORK/stage/lib/liblance_c.so"
    local expected actual
    expected=$(sed '/^[[:space:]]*\*/d' "$header" |
        grep -oE 'lance_[A-Za-z0-9_]+[[:space:]]*\(' |
        sed 's/[[:space:]]*($//' | sort -u)
    actual=$(nm -D --defined-only -j "$library" |
        sed 's/@.*//' | sort -u)
    printf '%s\n' "$expected" >"$EVIDENCE/header-exports.txt"
    printf '%s\n' "$actual" >"$EVIDENCE/shared-exports.txt"
    assert_eq "$(printf '%s\n' "$actual" | wc -l)" \
        "$(printf '%s\n' "$expected" | wc -l)" \
        "defined dynamic export count"
    printf '%s\n' "$actual" | grep -v '^lance_' \
        >"$EVIDENCE/non-lance-exports.txt" || true
    [ ! -s "$EVIDENCE/non-lance-exports.txt" ] ||
        fail "shared library exports non-lance symbols"
    cmp -s "$EVIDENCE/header-exports.txt" "$EVIDENCE/shared-exports.txt" ||
        fail "export set differs from the installed header"
    {
        echo "header_declarations=$(wc -l <"$EVIDENCE/header-exports.txt")"
        echo "defined_dynamic_exports=$(wc -l <"$EVIDENCE/shared-exports.txt")"
    } >"$EVIDENCE/export-count.txt"
}

verify_installed_headers() {
    diff -r "$SOURCE/include/lance" "$WORK/stage/include/lance"
    cmp "$SOURCE/include/lance/lance.h" \
        "$WORK/stage/include/lance/lance.h"
}

verify_linux_staging() {
    local prefix=$1
    bash "$SOURCE/.github/scripts/verify-linux-staging.sh" "$prefix" "$TARGET"
}

elf_metadata() {
    local prefix=$1
    local label=$2
    local library="$prefix/lib/liblance_c.so"
    local versions max_glibc

    readelf -h "$library" >"$EVIDENCE/$label-readelf-header.txt"
    readelf -d "$library" >"$EVIDENCE/$label-readelf-dynamic.txt"
    readelf -l "$library" >"$EVIDENCE/$label-readelf-program-headers.txt"
    readelf --version-info "$library" >"$EVIDENCE/$label-readelf-version-info.txt"
    ldd "$library" >"$EVIDENCE/$label-ldd.txt" 2>&1 || true
    file "$library" >"$EVIDENCE/$label-file-shared.txt" 2>&1 || \
        echo "file=unavailable" >"$EVIDENCE/$label-file-shared.txt"

    grep -q 'Machine:.*X86-64' "$EVIDENCE/$label-readelf-header.txt" ||
        fail "ELF machine is not X86-64"
    versions="$EVIDENCE/$label-glibc-versions.txt"
    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/$label-readelf-version-info.txt" | sort -uV >"$versions"
    [ -s "$versions" ] || fail "no GLIBC symbol versions found"
    max_glibc=$(tail -1 "$versions" | sed 's/^GLIBC_//')
    echo "$max_glibc" >"$EVIDENCE/$label-glibc-max.txt"
    version_le "$max_glibc" "2.28" ||
        fail "shared library requires GLIBC_$max_glibc, above 2.28"

    grep -oE 'GCC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/$label-readelf-version-info.txt" | sort -uV \
        >"$EVIDENCE/$label-gcc-versions.txt" || true
    if grep -Eq 'GLIBCXX_|CXXABI_' \
        "$EVIDENCE/$label-readelf-version-info.txt"; then
        fail "shared library references a C++ ABI symbol version"
    fi
    sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' \
        "$EVIDENCE/$label-readelf-dynamic.txt" \
        >"$EVIDENCE/$label-dt-needed.txt"
    [ -s "$EVIDENCE/$label-dt-needed.txt" ] ||
        fail "shared library has no DT_NEEDED entries"
}

verify_cxx17_headers() {
    local prefix=$1
    "$CXX" -std=c++17 -fsyntax-only \
        -I"$prefix/include" \
        "$SOURCE/examples/cmake-consumer/main.cpp" \
        "$SOURCE/examples/cmake-consumer/acceptance.cpp"
}

run_installed_consumer() {
    bash "$SOURCE/.github/scripts/verify-cmake-consumer.sh" \
        --from-install-prefix "$WORK/stage"
}

verify_consumer_log() {
    local log=$1
    local static_reads shared_reads passes clone_lines error_lines
    local scan_lines create_lines

    static_reads=$(grep -c \
        'Verified static consumer dataset read: rows: 512, version: 2' \
        "$log" || true)
    shared_reads=$(grep -c \
        'Verified shared consumer dataset read: rows: 512, version: 2' \
        "$log" || true)
    passes=$(grep -c 'acceptance: PASS' "$log" || true)
    clone_lines=$(grep -c \
        'acceptance: clone rows=4 source_closed_before_use=yes' "$log" || true)
    error_lines=$(grep -c \
        'acceptance: last_error code=3 message_bytes=' "$log" || true)
    scan_lines=$(grep -c \
        'acceptance: scan rows=4 batches=2 array_releases=2 schema_releases=1 stream_releases=1' \
        "$log" || true)
    create_lines=$(grep -c 'acceptance: create rows=4 version=1' \
        "$log" || true)

    assert_eq "$static_reads" "1" "static historical dataset reads"
    assert_eq "$shared_reads" "1" "shared historical dataset reads"
    assert_eq "$passes" "2" "acceptance PASS lines"
    assert_eq "$clone_lines" "2" "clone-lifetime acceptance lines"
    assert_eq "$error_lines" "2" "last-error acceptance lines"
    assert_eq "$create_lines" "2" "create acceptance lines"
    [ "$scan_lines" -ge 4 ] ||
        fail "expected at least four scan acceptance lines, found $scan_lines"
}

archive_version() {
    sed -n 's/^version = "\([^"]*\)"/\1/p' "$SOURCE/Cargo.toml" | head -1
}

make_archive() {
    local version archive
    version=$(archive_version)
    assert_nonempty "$version" "workspace version was not found"
    archive="$WORK/artifacts/lance-c-v${version}-${TARGET}.tar.xz"
    mkdir -p "$WORK/artifacts"
    rm -f "$archive" "$archive.sha256" "$archive.sha512"
    XZ_OPT=-T0 tar -C "$WORK/stage" -cJf "$archive" .
    xz -t "$archive"
    (
        cd "$WORK/artifacts"
        sha256sum "$(basename "$archive")" >"$(basename "$archive").sha256"
        sha512sum "$(basename "$archive")" >"$(basename "$archive").sha512"
    )
    file "$archive" >"$EVIDENCE/archive-file.txt" 2>&1 || true
    stat -c '%n %s' "$archive" >"$EVIDENCE/archive-size.txt"
    tar -tJf "$archive" >"$EVIDENCE/archive-members.txt"
    {
        echo "archive=$archive"
        echo "archive_name=$(basename "$archive")"
        cat "$EVIDENCE/archive-size.txt"
        cat "$archive.sha256"
        cat "$archive.sha512"
    } >"$EVIDENCE/archive-identity.txt"
}

extract_archive() {
    local archive
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    assert_file "$archive"
    rm -rf "$WORK/extracted"
    mkdir -p "$WORK/extracted"
    tar -xJf "$archive" -C "$WORK/extracted"
}

verify_extracted_archive_matches_stage() {
    manifest_tree "$WORK/extracted" "$EVIDENCE/extracted-manifest.sha256"
    cmp "$EVIDENCE/stage-manifest.sha256" \
        "$EVIDENCE/extracted-manifest.sha256" ||
        fail "extracted archive manifest differs from installed stage"
}

record_archive_audit() {
    local archive so a member_count machine_count
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    a="$WORK/extracted/lib/liblance_c.a"
    so="$WORK/extracted/lib/liblance_c.so"
    member_count=$(ar t "$a" | wc -l)
    readelf -h "$a" >"$EVIDENCE/archive-static-readelf-header.txt" 2>&1 || true
    machine_count=$(grep -c 'Machine:.*X86-64' \
        "$EVIDENCE/archive-static-readelf-header.txt" || true)
    assert_nonempty "$member_count" "static archive has no members"
    [ "$member_count" -gt 0 ] || fail "static archive has zero members"
    assert_eq "$machine_count" "$member_count" \
        "static archive members that are x86_64 ELF objects"
    if ar t "$a" | grep -Eq '(^/|/\.\./|^\.\./)'; then
        fail "static archive contains an absolute or parent-relative member name"
    fi
    readelf --version-info "$a" \
        >"$EVIDENCE/archive-static-version-info.txt" 2>&1 || true
    {
        echo "archive_member_count=$member_count"
        echo "archive_machine_count=$machine_count"
        echo "archive_member_architecture=x86_64"
        echo "archive_static_sha256=$(sha256sum "$a" | awk '{print $1}')"
        echo "archive_shared_sha256=$(sha256sum "$so" | awk '{print $1}')"
        echo "archive_static_bytes=$(stat -c %s "$a")"
        echo "archive_shared_bytes=$(stat -c %s "$so")"
        echo "shared_max_glibc=$(cat "$EVIDENCE/extracted-glibc-max.txt")"
        echo "static_consumer_max_glibc=$(cat "$EVIDENCE/consumer-static-glibc-max.txt")"
        echo "shared_dt_needed_begin"
        cat "$EVIDENCE/extracted-dt-needed.txt"
        echo "shared_dt_needed_end"
    } >"$EVIDENCE/archive-audit.txt"
}

disable_original_prefix() {
    [ -d "$WORK/stage" ] || fail "original install prefix is missing"
    mv "$WORK/stage" "$WORK/stage-unavailable"
    [ ! -e "$WORK/stage" ] ||
        fail "original install prefix is still available at $WORK/stage"
    [ -d "$WORK/stage-unavailable" ] ||
        fail "original install prefix was not preserved for audit"
    {
        echo "original_prefix=$WORK/stage"
        echo "original_prefix_state=unavailable"
        echo "preserved_as=$WORK/stage-unavailable"
    } >"$EVIDENCE/prefix-unavailable.txt"
}

verify_glibc_for_executable() {
    local exe=$1
    local label=$2
    local versions max_glibc

    versions="$EVIDENCE/${label}-glibc-versions.txt"
    readelf --version-info "$exe" >"$EVIDENCE/${label}-readelf-version-info.txt"
    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/${label}-readelf-version-info.txt" | sort -uV >"$versions"
    [ -s "$versions" ] || fail "no GLIBC versions found in $exe"
    max_glibc=$(tail -1 "$versions" | sed 's/^GLIBC_//')
    echo "$max_glibc" >"$EVIDENCE/${label}-glibc-max.txt"
    version_le "$max_glibc" "2.28" ||
        fail "$exe requires GLIBC_$max_glibc, above 2.28"
}

verify_no_original_paths() {
    local prefix_hits metadata_hits source_metadata_hits
    local so_build_paths a_build_paths

    prefix_hits=$(grep -R -a -l -F "$WORK/stage" "$WORK/extracted" || true)
    assert_eq "$prefix_hits" "" "extracted payload references original prefix"

    metadata_hits=$(grep -R -a -l -F "$WORK/build" "$WORK/extracted" \
        --include='*.cmake' --include='*.pc' --include='*.h' \
        --include='*.hpp' || true)
    assert_eq "$metadata_hits" "" "package metadata references build directory"

    source_metadata_hits=$(grep -R -a -l -F "$SOURCE" "$WORK/extracted" \
        --include='*.cmake' --include='*.pc' --include='*.h' \
        --include='*.hpp' || true)
    assert_eq "$source_metadata_hits" "" "package metadata references source directory"

    so_build_paths=$({ grep -a -o -F "$WORK/build" \
        "$WORK/extracted/lib/liblance_c.so" || true; } | wc -l)
    a_build_paths=$({ grep -a -o -F "$WORK/build" \
        "$WORK/extracted/lib/liblance_c.a" || true; } | wc -l)
    {
        echo "original_prefix_path_hits=0"
        echo "build_path_metadata_hits=0"
        echo "source_path_metadata_hits=0"
        echo "binary_generated_build_path_strings_so=$so_build_paths"
        echo "binary_generated_build_path_strings_a=$a_build_paths"
        echo "note=compiler-generated Rust out/*.rs strings remain in product objects; no install or package path references"
    } >"$EVIDENCE/path-audit.txt"
}

extracted_consumer() {
    local linkage=$1
    local build_dir="$WORK/consumers/extracted-$linkage"
    local combined="$EVIDENCE/logs/consumer-extracted.log"
    local exe="$build_dir/consumer"
    local acceptance="$build_dir/acceptance"
    local dataset_uri="$SOURCE/test_data/v0.27.1/pq_in_schema"
    local acceptance_path="$WORK/datasets/extracted-${linkage}.lance"
    local ld_env
    local dataset_output rc
    local link_txt="$build_dir/CMakeFiles/consumer.dir/link.txt"
    local link_acceptance="$build_dir/CMakeFiles/acceptance.dir/link.txt"

    rm -rf "$build_dir" "$acceptance_path"
    mkdir -p "$build_dir" "$WORK/datasets"

    printf '==> extracted %s find_package configure\n' "$linkage" >>"$combined"
    cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
        -DCMAKE_PREFIX_PATH="$WORK/extracted" \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SKIP_RPATH=ON \
        -DLanceC_LINK="$linkage" >>"$combined" 2>&1
    grep -Fq "LanceC_DIR:PATH=$WORK/extracted/lib/cmake/LanceC" \
        "$build_dir/CMakeCache.txt" ||
        fail "$linkage consumer did not resolve LanceC from extracted prefix"
    printf '==> extracted %s build\n' "$linkage" >>"$combined"
    cmake --build "$build_dir" --parallel 2 >>"$combined" 2>&1

    assert_file "$link_txt"
    assert_file "$link_acceptance"
    if grep -Fq "$WORK/stage" "$link_txt" "$link_acceptance"; then
        fail "$linkage consumer link command references the unavailable prefix"
    fi
    if [ "$linkage" = static ]; then
        grep -Fq "$WORK/extracted/lib/liblance_c.a" "$link_txt" ||
            fail "static consumer does not link the extracted static archive"
    else
        grep -Fq "$WORK/extracted/lib/liblance_c.so" "$link_txt" ||
            grep -Fq -- "-L$WORK/extracted/lib" "$link_txt" ||
            fail "shared consumer does not link from the extracted prefix"
    fi

    if [ "$linkage" = static ]; then
        ld_env=(env -u LD_LIBRARY_PATH)
    else
        ld_env=(env "LD_LIBRARY_PATH=$WORK/extracted/lib")
    fi

    printf '==> extracted %s usage check\n' "$linkage" >>"$combined"
    set +e
    "${ld_env[@]}" "$exe" >>"$combined" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" "2" "$linkage consumer usage exit code"

    printf '==> extracted %s dataset check\n' "$linkage" >>"$combined"
    set +e
    dataset_output=$("${ld_env[@]}" "$exe" "$dataset_uri" 2>&1)
    rc=$?
    set -e
    printf '%s\n' "$dataset_output" >>"$combined"
    assert_eq "$rc" "0" "$linkage consumer dataset exit code"
    assert_eq "$dataset_output" "rows: 512, version: 2" \
        "$linkage consumer dataset output"

    printf '==> extracted %s acceptance check\n' "$linkage" >>"$combined"
    "${ld_env[@]}" "$acceptance" "file://$acceptance_path" >>"$combined" 2>&1

    "${ld_env[@]}" ldd "$exe" \
        >"$EVIDENCE/consumer-${linkage}-ldd.txt" 2>&1 || true
    "${ld_env[@]}" ldd "$acceptance" \
        >"$EVIDENCE/acceptance-${linkage}-ldd.txt" 2>&1 || true
    readelf -d "$exe" >"$EVIDENCE/consumer-${linkage}-readelf-dynamic.txt"
    readelf -d "$acceptance" \
        >"$EVIDENCE/acceptance-${linkage}-readelf-dynamic.txt"

    if [ "$linkage" = shared ]; then
        grep -Fq "liblance_c.so => $WORK/extracted/lib/liblance_c.so" \
            "$EVIDENCE/consumer-${linkage}-ldd.txt" ||
            fail "shared consumer does not resolve liblance_c.so from extracted prefix"
        grep -Fq 'liblance_c.so' \
            "$EVIDENCE/consumer-${linkage}-readelf-dynamic.txt" ||
            fail "shared consumer has no liblance_c.so DT_NEEDED"
        if grep -Fq "$WORK/stage" "$EVIDENCE/consumer-${linkage}-ldd.txt"; then
            fail "shared consumer runtime resolves through the unavailable prefix"
        fi
    else
        if grep -Fq 'liblance_c.so' \
            "$EVIDENCE/consumer-${linkage}-ldd.txt" ||
            grep -Fq 'liblance_c.so' \
                "$EVIDENCE/consumer-${linkage}-readelf-dynamic.txt" ||
            grep -Fq 'liblance_c.so' \
                "$EVIDENCE/acceptance-${linkage}-readelf-dynamic.txt"; then
            fail "static consumer has a liblance_c.so dependency"
        fi
    fi

    verify_glibc_for_executable "$exe" "consumer-$linkage"
    verify_glibc_for_executable "$acceptance" "acceptance-$linkage"
    {
        echo "linkage=$linkage"
        echo "build_dir=$build_dir"
        echo "prefix=$WORK/extracted"
        echo "link_command_begin"
        cat "$link_txt"
        echo "link_command_end"
        echo "ldd_begin"
        cat "$EVIDENCE/consumer-${linkage}-ldd.txt"
        echo "ldd_end"
        echo "dt_needed_begin"
        sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' \
            "$EVIDENCE/consumer-${linkage}-readelf-dynamic.txt"
        echo "dt_needed_end"
        echo "max_glibc=$(cat "$EVIDENCE/consumer-${linkage}-glibc-max.txt")"
    } >"$EVIDENCE/consumer-${linkage}.txt"
}

verify_pkgconfig() {
    local pc_dir="$WORK/extracted/lib/pkgconfig"
    local combined="$EVIDENCE/logs/pkgconfig-consumer.log"
    local cflags libs exe="$WORK/pkgconfig-consumer"
    local version output rc

    printf '==> pkg-config metadata\n' >"$combined"
    version=$(PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_LIBDIR="$pc_dir" \
        pkg-config --modversion lance-c)
    cflags=$(PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_LIBDIR="$pc_dir" \
        pkg-config --cflags lance-c)
    libs=$(PKG_CONFIG_PATH="$pc_dir" PKG_CONFIG_LIBDIR="$pc_dir" \
        pkg-config --libs lance-c)
    {
        echo "version=$version"
        echo "cflags=$cflags"
        echo "libs=$libs"
    } >>"$combined"
    assert_eq "$version" "0.1.9" "pkg-config version"
    printf '%s\n' "$cflags" | grep -Fq "$WORK/extracted/include" ||
        fail "pkg-config include path is not the extracted prefix"
    printf '%s\n' "$libs" | grep -Fq "$WORK/extracted/lib" ||
        fail "pkg-config library path is not the extracted prefix"
    if printf '%s\n%s\n' "$cflags" "$libs" | grep -Fq "$WORK/stage"; then
        fail "pkg-config metadata references the unavailable prefix"
    fi

    printf '==> pkg-config compile and run\n' >>"$combined"
    # shellcheck disable=SC2086
    "$CXX" -std=c++17 $cflags "$SOURCE/examples/cmake-consumer/main.cpp" \
        $libs -o "$exe" >>"$combined" 2>&1
    set +e
    output=$(LD_LIBRARY_PATH="$WORK/extracted/lib" "$exe" \
        "$SOURCE/test_data/v0.27.1/pq_in_schema" 2>&1)
    rc=$?
    set -e
    printf '%s\n' "$output" >>"$combined"
    assert_eq "$rc" "0" "pkg-config consumer exit code"
    assert_eq "$output" "rows: 512, version: 2" "pkg-config consumer output"
}

verify_source_unchanged() {
    local dirty
    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source changed during validation"
}

write_summary() {
    local static_sha static_bytes shared_sha shared_bytes
    local archive archive_sha archive_bytes exports dt_needed
    local stage_glibc static_glibc so_build_paths a_build_paths
    local member_count

    static_sha=$(sha256sum "$WORK/extracted/lib/liblance_c.a" |
        awk '{print $1}')
    static_bytes=$(stat -c %s "$WORK/extracted/lib/liblance_c.a")
    shared_sha=$(sha256sum "$WORK/extracted/lib/liblance_c.so" |
        awk '{print $1}')
    shared_bytes=$(stat -c %s "$WORK/extracted/lib/liblance_c.so")
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    archive_sha=$(awk '{print $1}' "$archive.sha256")
    archive_bytes=$(stat -c %s "$archive")
    exports=$(grep '^Verified ' "$LOG_DIR/exports.log" | tail -1)
    assert_nonempty "$exports" "export verification output missing"
    dt_needed=$(paste -sd, "$EVIDENCE/extracted-dt-needed.txt")
    stage_glibc=$(cat "$EVIDENCE/stage-glibc-max.txt")
    static_glibc=$(cat "$EVIDENCE/consumer-static-glibc-max.txt")
    so_build_paths=$(sed -n \
        's/^binary_generated_build_path_strings_so=//p' "$EVIDENCE/path-audit.txt")
    a_build_paths=$(sed -n \
        's/^binary_generated_build_path_strings_a=//p' "$EVIDENCE/path-audit.txt")
    member_count=$(sed -n 's/^archive_member_count=//p' \
        "$EVIDENCE/archive-audit.txt")

    {
        echo "status=pass"
        echo "product_commit=$PRODUCT_COMMIT"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "validation_parent=$(git -C "$SOURCE" rev-parse "$VALIDATION_COMMIT^")"
        echo "product_files_changed=$(git -C "$SOURCE" diff --name-only \
            "$PRODUCT_COMMIT" "$VALIDATION_COMMIT" -- \
            src include Cargo.toml Cargo.lock build.rs CMakeLists.txt cmake \
            .github/scripts examples/cmake-consumer test_data .github/workflows |
            wc -l)"
        echo "image=$IMAGE@$IMAGE_DIGEST"
        echo "image_id=$(sed -n 's/^image_id=//p' \
            "$EVIDENCE/orchestration-identity.txt")"
        echo "container_hostname=$(uname -n)"
        echo "container_arch=$(uname -m)"
        echo "container_libc=$(getconf GNU_LIBC_VERSION)"
        echo "rustc=$(rustc -V)"
        echo "cargo=$(cargo --version)"
        echo "rustup_toolchain=${RUSTUP_TOOLCHAIN-}"
        echo "clang=$($CC --version | awk 'NR == 1')"
        echo "cmake=$(cmake --version | awk 'NR == 1')"
        echo "protoc=$(protoc --version)"
        echo "cargo_build_jobs=${CARGO_BUILD_JOBS-}"
        echo "rust_test_threads=${RUST_TEST_THREADS-}"
        echo "cmake_build_parallel_level=${CMAKE_BUILD_PARALLEL_LEVEL-}"
        echo "cargo_profile_release_lto=${CARGO_PROFILE_RELEASE_LTO-unset}"
        echo "cmake_install_libdir=$(sed -n 's/^cmake_install_libdir=//p' \
            "$EVIDENCE/stage-install-layout.txt")"
        echo "installed_libdir=lib"
        echo "installed_lib_is_symlink=no"
        echo "installed_lib64_exists=no"
        echo "installed_symlink_count=0"
        echo "build_reused=$RESUME"
        echo "static_sha256=$static_sha"
        echo "static_bytes=$static_bytes"
        echo "shared_stripped_sha256=$shared_sha"
        echo "shared_stripped_bytes=$shared_bytes"
        echo "exports=$exports"
        echo "header_declarations=$(sed -n 's/^header_declarations=//p' \
            "$EVIDENCE/export-count.txt")"
        echo "defined_dynamic_exports=$(sed -n \
            's/^defined_dynamic_exports=//p' "$EVIDENCE/export-count.txt")"
        echo "shared_max_glibc=$stage_glibc"
        echo "static_consumer_max_glibc=$static_glibc"
        echo "shared_dt_needed=$dt_needed"
        echo "cxx17_headers=pass"
        echo "consumer_static_dataset=rows:512,version:2"
        echo "consumer_shared_dataset=rows:512,version:2"
        echo "consumer_acceptance=PASS"
        echo "consumer_clone_after_source_close=yes"
        echo "consumer_last_error_ownership=consumed"
        echo "consumer_arrow_release_counts=2/1/1"
        echo "archive=$archive"
        echo "archive_sha256=$archive_sha"
        echo "archive_bytes=$archive_bytes"
        echo "archive_member_count=$member_count"
        echo "archive_member_architecture=x86_64"
        echo "extracted_prefix=$WORK/extracted"
        echo "original_prefix_state=unavailable"
        echo "extracted_static_dataset=rows:512,version:2"
        echo "extracted_shared_dataset=rows:512,version:2"
        echo "extracted_acceptance=PASS"
        echo "shared_runtime_binding=$WORK/extracted/lib/liblance_c.so"
        echo "static_no_liblance_so_dependency=yes"
        echo "original_prefix_path_hits=0"
        echo "build_path_metadata_hits=0"
        echo "binary_generated_build_path_strings_so=$so_build_paths"
        echo "binary_generated_build_path_strings_a=$a_build_paths"
        echo "source_unchanged=yes"
    } >"$EVIDENCE/summary.env"
}

container_body() {
    [ "$(id -u)" = 1000 ] || fail "container body must run as uid 1000"
    SOURCE=$(cd "$SOURCE" && pwd)
    WORK=$(cd "$WORK" && pwd)
    EVIDENCE=$(cd "$EVIDENCE" && pwd)
    export CC=/opt/clang/bin/clang
    export CXX=/opt/clang/bin/clang++
    export CARGO_BUILD_JOBS=8
    export RUST_TEST_THREADS=4
    export CMAKE_BUILD_PARALLEL_LEVEL=1
    export RUSTUP_TOOLCHAIN=1.94.0
    unset CARGO_PROFILE_RELEASE_LTO RUSTFLAGS CARGO_TARGET_DIR 2>/dev/null || true

    setup_status
    record_environment
    record_source_identity
    assert_eq "$(rustc -V | awk '{print $2}')" "1.94.0" "rustc version"
    assert_eq "$(cargo --version | awk '{print $2}')" "1.94.0" \
        "cargo version"
    assert_eq "$(getconf GNU_LIBC_VERSION)" "glibc 2.28" \
        "container glibc"
    assert_eq "$(uname -m)" "x86_64" "container architecture"

    if [ "$RESUME" = 1 ]; then
        if [ ! -e "$WORK/stage" ] && [ -d "$WORK/stage-unavailable" ]; then
            mv "$WORK/stage-unavailable" "$WORK/stage"
        fi
        [ -f "$WORK/build/CMakeCache.txt" ] ||
            fail "resume requires a preserved CMake cache"
        [ -d "$WORK/stage/lib" ] ||
            fail "resume requires a preserved installed stage"
        local cache_libdir
        cache_libdir=$(sed -n 's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' \
            "$WORK/build/CMakeCache.txt" | tail -1)
        assert_eq "$cache_libdir" "lib" "resumed CMAKE_INSTALL_LIBDIR"
    else
        [ ! -e "$WORK/build" ] ||
            fail "work directory already contains a build: $WORK/build"
        [ ! -e "$WORK/stage" ] ||
            fail "work directory already contains a stage: $WORK/stage"
        run_step configure-release configure_release
        run_step build-release build_release
        run_step install-release install_release
    fi

    run_step record-artifacts record_artifacts
    run_step install-layout record_install_layout "$WORK/stage" stage
    run_step stage-prestrip record_stage_prestrip
    run_step strip-shared strip_shared
    run_step stage-poststrip record_stage_poststrip
    run_step static-untouched verify_static_untouched
    run_step exports verify_exports
    run_step export-sets record_export_sets
    run_step installed-headers verify_installed_headers
    run_step linux-staging verify_linux_staging "$WORK/stage"
    run_step elf-stage-record elf_metadata "$WORK/stage" stage
    run_step cxx17-headers verify_cxx17_headers "$WORK/stage"
    run_step consumer-installed run_installed_consumer
    verify_consumer_log "$LOG_DIR/consumer-installed.log"
    run_step make-archive make_archive
    run_step extract-archive extract_archive
    run_step extracted-layout record_install_layout "$WORK/extracted" extracted
    run_step extracted-manifest verify_extracted_archive_matches_stage
    run_step disable-original-prefix disable_original_prefix
    run_step path-audit verify_no_original_paths
    run_step consumer-static extracted_consumer static
    run_step consumer-shared extracted_consumer shared
    verify_consumer_log "$LOG_DIR/consumer-extracted.log"
    run_step pkgconfig verify_pkgconfig
    run_step linux-staging-extracted verify_linux_staging "$WORK/extracted"
    run_step extracted-exports verify_extracted_exports
    run_step elf-extracted elf_metadata "$WORK/extracted" extracted
    run_step archive-audit record_archive_audit
    run_step source-unchanged verify_source_unchanged
    run_step summary write_summary
    append_status ACCEPTED "issue103 install/extracted acceptance complete"
}

run_outer() {
    local image_ref="$IMAGE@$IMAGE_DIGEST"
    local image_id repo_digests
    local rc=0

    SOURCE=$(cd "$SOURCE" && pwd)
    ROOT=$(mkdir -p "$ROOT" && cd "$ROOT" && pwd)
    EVIDENCE="$ROOT/evidence"
    WORK="$ROOT/work"
    mkdir -p "$EVIDENCE" "$WORK"
    if [ -e "$EVIDENCE/commands-status.txt" ]; then
        [ "$RESUME" = 1 ] ||
            fail "evidence already exists: $EVIDENCE"
        echo "RESUME $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            >>"$EVIDENCE/orchestration-status.txt"
    else
        : >"$EVIDENCE/orchestration-status.txt"
    fi
    LOG_DIR="$EVIDENCE/logs"
    mkdir -p "$LOG_DIR"
    VALIDATION_COMMIT=$(git -C "$SOURCE" rev-parse HEAD)
    CONTAINER_NAME="lance103-lib-acceptance-$(date -u +%Y%m%dT%H%M%SZ)-$$"

    [ "$(uname -m)" = x86_64 ] || fail "Nier host is not x86_64"
    command -v docker >/dev/null || fail "docker is unavailable"
    docker version >"$EVIDENCE/docker-version.txt"
    docker pull "$image_ref" 2>&1 | tee "$EVIDENCE/docker-pull.log"
    image_id=$(docker image inspect --format '{{.Id}}' "$image_ref")
    repo_digests=$(docker image inspect \
        --format '{{range .RepoDigests}}{{println .}}{{end}}' "$image_ref")
    {
        echo "host_hostname=$(uname -n)"
        echo "host_uname=$(uname -a)"
        echo "host_arch=$(uname -m)"
        echo "host_os_release_begin"
        cat /etc/os-release
        echo "host_os_release_end"
        echo "image_ref=$image_ref"
        echo "image_id=$image_id"
        echo "image_repo_digests=$repo_digests"
        echo "clang_version=$CLANG_VERSION"
        echo "protoc_version=$PROTOC_VERSION"
        echo "protoc_sha256=$PROTOC_SHA256"
        echo "product_commit=$PRODUCT_COMMIT"
        echo "validation_commit=$VALIDATION_COMMIT"
    } >"$EVIDENCE/orchestration-identity.txt"
    echo "START $(date -u +%Y-%m-%dT%H:%M:%SZ) docker-create" \
        >>"$EVIDENCE/orchestration-status.txt"
    docker run -d --name "$CONTAINER_NAME" \
        -e HOME=/home/xuanwo \
        -e CARGO_HOME=/home/xuanwo/.cargo \
        -e RUSTUP_HOME=/home/xuanwo/.rustup \
        -e XDG_CACHE_HOME=/home/xuanwo/.cache \
        -e XDG_CONFIG_HOME=/home/xuanwo/.config \
        -e GIT_OPTIONAL_LOCKS=0 \
        -e PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.cargo/bin:/home/xuanwo/.local/bin:/opt/clang/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        -e GIT_CONFIG_COUNT=1 \
        -e GIT_CONFIG_KEY_0=safe.directory \
        -e GIT_CONFIG_VALUE_0='*' \
        -v "$SOURCE:/source:ro" \
        -v "$WORK:/work" \
        -v "$EVIDENCE:/evidence" \
        -v "$SCRIPT_PATH:/driver:ro" \
        -v /home/xuanwo/.local/bin/mbx:/home/xuanwo/.local/bin/mbx:ro \
        -v /home/xuanwo/.local/share/mbx:/home/xuanwo/.local/share/mbx:ro \
        -v /home/xuanwo/.cargo:/home/xuanwo/.cargo:rw \
        -v /home/xuanwo/.rustup:/home/xuanwo/.rustup:ro \
        -v /home/xuanwo/.cache/mbx:/home/xuanwo/.cache/mbx:rw \
        -v /home/xuanwo/.config/mbx:/home/xuanwo/.config/mbx:ro \
        "$image_ref" bash -lc 'trap "exit 0" TERM INT; sleep infinity & wait' \
        >>"$EVIDENCE/orchestration-status.txt"
    echo "PASS docker-create" >>"$EVIDENCE/orchestration-status.txt"
    {
        echo "container_name=$CONTAINER_NAME"
        echo "source_mount=$SOURCE:/source:ro"
        echo "work_mount=$WORK:/work"
        echo "evidence_mount=$EVIDENCE:/evidence"
        echo "driver_mount=$SCRIPT_PATH:/driver:ro"
        echo "mbx_cache_mount=/home/xuanwo/.cache/mbx"
        echo "rustup_mount=/home/xuanwo/.rustup:ro"
        echo "cargo_home_mount=/home/xuanwo/.cargo:rw"
    } >"$EVIDENCE/orchestration.txt"

    set +e
    docker exec -u 0 "$CONTAINER_NAME" bash /driver \
        --container-setup --evidence /evidence \
        2>&1 | tee -a "$EVIDENCE/container-setup.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ]; then
        echo "FAIL container-setup rc=$rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        docker logs "$CONTAINER_NAME" >"$EVIDENCE/container.log" 2>&1 || true
        docker inspect "$CONTAINER_NAME" >"$EVIDENCE/container-inspect.json" || true
        echo "container setup failed with rc=$rc; container $CONTAINER_NAME retained" \
            >"$EVIDENCE/FAILURE.txt"
        exit "$rc"
    fi
    echo "PASS container-setup" >>"$EVIDENCE/orchestration-status.txt"

    set +e
    docker exec --user 1000:1000 \
        -e HOME=/home/xuanwo \
        -e CARGO_HOME=/home/xuanwo/.cargo \
        -e RUSTUP_HOME=/home/xuanwo/.rustup \
        -e XDG_CACHE_HOME=/home/xuanwo/.cache \
        -e XDG_CONFIG_HOME=/home/xuanwo/.config \
        -e GIT_OPTIONAL_LOCKS=0 \
        -e PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.cargo/bin:/home/xuanwo/.local/bin:/opt/clang/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        -e GIT_CONFIG_COUNT=1 \
        -e GIT_CONFIG_KEY_0=safe.directory \
        -e GIT_CONFIG_VALUE_0='*' \
        "$CONTAINER_NAME" bash /driver \
        --container-body \
        --source /source \
        --work /work \
        --evidence /evidence \
        --product-commit "$PRODUCT_COMMIT" \
        --validation-commit "$VALIDATION_COMMIT" \
        "${resume_args[@]}" \
        2>&1 | tee -a "$EVIDENCE/container-run.log"
    rc=${PIPESTATUS[0]}
    set -e

    docker logs "$CONTAINER_NAME" >"$EVIDENCE/container.log" 2>&1 || true
    docker inspect "$CONTAINER_NAME" >"$EVIDENCE/container-inspect.json" || true
    docker exec -u 0 "$CONTAINER_NAME" chown -R 1000:1000 /evidence /work \
        >/dev/null 2>&1 || true

    if [ "$rc" -ne 0 ]; then
        echo "FAIL container-body rc=$rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        echo "container body failed with rc=$rc; container $CONTAINER_NAME retained" \
            >"$EVIDENCE/FAILURE.txt"
        exit "$rc"
    fi
    echo "PASS container-body" >>"$EVIDENCE/orchestration-status.txt"
    docker stop "$CONTAINER_NAME" >/dev/null
    docker rm "$CONTAINER_NAME" >/dev/null
    echo "DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >>"$EVIDENCE/orchestration-status.txt"
}

case "$MODE" in
    container-setup)
        [ -n "$EVIDENCE" ] || fail "container setup requires --evidence"
        container_setup
        ;;
    container-body)
        [ -n "$SOURCE" ] && [ -n "$WORK" ] && [ -n "$EVIDENCE" ] &&
            [ -n "$PRODUCT_COMMIT" ] && [ -n "$VALIDATION_COMMIT" ] ||
            fail "container body arguments are incomplete"
        container_body
        ;;
    outer)
        [ -n "$SOURCE" ] && [ -n "$ROOT" ] && [ -n "$PRODUCT_COMMIT" ] ||
            fail "outer arguments are incomplete"
        resume_args=()
        [ "$RESUME" = 1 ] && resume_args=(--resume)
        run_outer
        ;;
esac
