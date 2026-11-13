#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail
export LC_ALL=C

SCRIPT_PATH=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
MODE=outer
SOURCE=
ROOT=
WORK=
EVIDENCE=
PRODUCT_COMMIT=
VALIDATION_COMMIT=

IMAGE=quay.io/pypa/manylinux_2_28_aarch64
IMAGE_DIGEST=sha256:cb37f0d66c8a75abb93cf6085cad1f1427cd7208f0c0fc48359bb1eea9924f21
IMAGE_REF="$IMAGE@$IMAGE_DIGEST"
CLANG_VERSION=v20.1.8.0
CLANG_VERSION_TEXT="clang version 20.1.8"
PROTOC_VERSION=25.3
PROTOC_ASSET=protoc-25.3-linux-aarch_64.zip
PROTOC_SHA256=9eae1f20f70cccc912d1c318c3929b86aebf5afd4b0f32c196ef682c222ed5ae
RUSTUP_SHA256=9f608c142e0e38a78957d0661ba1dfaf0b6fb37184cfaa4b9a0aa57af6184282
TARGET=aarch64-unknown-linux-gnu
EXPECTED_EXPORTS=878
VALIDATION_DIR=validation/manylinux103-arm64-feasibility
WORKFLOW_PATH=.github/workflows/manylinux103-arm64-feasibility.yml
CONTAINER_NAME=
STATUS_FILE=
LOG_DIR=

usage() {
    cat <<'EOF'
usage:
  run.sh --source <git-root> --root <evidence-root> --product-commit <sha>
  run.sh --container-setup --work <dir> --evidence <dir>
  run.sh --container-body --source <dir> --work <dir> --evidence <dir> \
    --product-commit <sha> --validation-commit <sha>
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --source) SOURCE=$2; shift 2 ;;
        --root) ROOT=$2; shift 2 ;;
        --work) WORK=$2; shift 2 ;;
        --evidence) EVIDENCE=$2; shift 2 ;;
        --product-commit) PRODUCT_COMMIT=$2; shift 2 ;;
        --validation-commit) VALIDATION_COMMIT=$2; shift 2 ;;
        --container-setup) MODE=container-setup; shift ;;
        --container-body) MODE=container-body; shift ;;
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
    printf '%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        "$state" "$*" >>"$STATUS_FILE"
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

path_tree() {
    local root=$1
    local out=$2
    (
        cd "$root"
        find . -mindepth 1 | sort
    ) >"$out"
}

setup_status() {
    STATUS_FILE="$EVIDENCE/commands-status.txt"
    LOG_DIR="$EVIDENCE/logs"
    mkdir -p "$LOG_DIR"
    [ ! -e "$STATUS_FILE" ] ||
        fail "evidence directory already contains a run: $EVIDENCE"
    : >"$STATUS_FILE"
    trap 'rc=$?; append_status SCRIPT_EXIT "rc=$rc" 2>/dev/null || true; exit "$rc"' EXIT
}

container_setup() {
    local tools=/opt/clang/bin
    local protoc_zip=/tmp/${PROTOC_ASSET}
    local rustup_init=/tmp/rustup-init-aarch64

    [ "$(id -u)" = 0 ] || fail "container setup must run as root"
    assert_eq "$(uname -m)" "aarch64" "container setup architecture"
    assert_eq "$(getconf GNU_LIBC_VERSION)" "glibc 2.28" \
        "container setup glibc"

    mkdir -p "$EVIDENCE" /work/toolchain/cargo /work/toolchain/rustup
    mkdir -p /work/toolchain/protoc/bin /work/toolchain/protoc/include
    git config --global --add safe.directory '*'

    {
        echo "setup_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "image=$IMAGE_REF"
        echo "container_arch=$(uname -m)"
        echo "container_glibc=$(getconf GNU_LIBC_VERSION)"
        echo "auditwheel_policy=${AUDITWHEEL_POLICY-}"
        echo "auditwheel_arch=${AUDITWHEEL_ARCH-}"
        echo "auditwheel_plat=${AUDITWHEEL_PLAT-}"
        echo "clang_installer=$(command -v manylinux-install-clang)"
        echo "clang_requested_version=$CLANG_VERSION"
        echo "protoc_asset=$PROTOC_ASSET"
        echo "protoc_sha256_expected=$PROTOC_SHA256"
        echo "rustup_init_sha256_expected=$RUSTUP_SHA256"
        echo "rustup_toolchain=1.94.0"
        echo "os_release_begin"
        cat /etc/os-release
        echo "os_release_end"
    } >"$EVIDENCE/setup-identity.txt"

    manylinux-install-clang -v "$CLANG_VERSION"
    [ -x "$tools/clang" ] || fail "Clang was not installed at $tools/clang"
    [ -x "$tools/clang++" ] || fail "Clang++ was not installed at $tools/clang++"
    "$tools/clang" --version >"$EVIDENCE/clang-version.txt"
    "$tools/clang++" --version >"$EVIDENCE/clangxx-version.txt"
    grep -Fq "$CLANG_VERSION_TEXT" "$EVIDENCE/clang-version.txt" ||
        fail "unexpected Clang version: $(head -1 "$EVIDENCE/clang-version.txt")"
    grep -Fq "$CLANG_VERSION_TEXT" "$EVIDENCE/clangxx-version.txt" ||
        fail "unexpected Clang++ version: $(head -1 "$EVIDENCE/clangxx-version.txt")"

    curl -fsSL --retry 3 -o "$protoc_zip" \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/${PROTOC_ASSET}"
    echo "$PROTOC_SHA256  $protoc_zip" | sha256sum -c -
    python3 -m zipfile -e "$protoc_zip" /usr/local
    chmod 0755 /usr/local/bin/protoc
    "$tools/clang" --version | head -1 >>"$EVIDENCE/setup-identity.txt"
    /usr/local/bin/protoc --version >"$EVIDENCE/protoc-version.txt"
    grep -Fq "libprotoc ${PROTOC_VERSION}" "$EVIDENCE/protoc-version.txt" ||
        fail "unexpected protoc version: $(cat "$EVIDENCE/protoc-version.txt")"

    curl -fsSL --retry 3 -o "$rustup_init" \
        "https://static.rust-lang.org/rustup/dist/aarch64-unknown-linux-gnu/rustup-init"
    echo "$RUSTUP_SHA256  $rustup_init" | sha256sum -c -
    chmod 0755 "$rustup_init"
    CARGO_HOME=/work/toolchain/cargo \
        RUSTUP_HOME=/work/toolchain/rustup \
        "$rustup_init" -y --profile minimal --default-toolchain 1.94.0 \
        --no-modify-path

    {
        echo "clang_actual=$(head -1 "$EVIDENCE/clang-version.txt")"
        echo "clangxx_actual=$(head -1 "$EVIDENCE/clangxx-version.txt")"
        echo "protoc_actual=$(cat "$EVIDENCE/protoc-version.txt")"
        echo "protoc_path=/usr/local/bin/protoc"
        echo "rustup_init_sha256_actual=$(sha256sum "$rustup_init" | awk '{print $1}')"
        echo "cargo_home=/work/toolchain/cargo"
        echo "rustup_home=/work/toolchain/rustup"
    } >>"$EVIDENCE/setup-identity.txt"

    CARGO_HOME=/work/toolchain/cargo \
        RUSTUP_HOME=/work/toolchain/rustup \
        PATH="/work/toolchain/cargo/bin:$PATH" \
        rustc -vV >"$EVIDENCE/rustc-setup.txt"
    CARGO_HOME=/work/toolchain/cargo \
        RUSTUP_HOME=/work/toolchain/rustup \
        PATH="/work/toolchain/cargo/bin:$PATH" \
        cargo --version >"$EVIDENCE/cargo-setup.txt"
    grep -Fq 'release: 1.94.0' "$EVIDENCE/rustc-setup.txt" ||
        fail "rustc setup did not install 1.94.0"
    grep -Fq 'host: aarch64-unknown-linux-gnu' "$EVIDENCE/rustc-setup.txt" ||
        fail "rustc setup host is not aarch64-unknown-linux-gnu"
    grep -Fq 'cargo 1.94.0' "$EVIDENCE/cargo-setup.txt" ||
        fail "cargo setup did not install 1.94.0"
}

record_environment() {
    local jobs test_threads
    jobs=$(nproc)
    [ "$jobs" -ge 1 ] || fail "nproc returned $jobs"
    [ "$jobs" -le 8 ] || jobs=8
    test_threads=$(nproc)
    [ "$test_threads" -ge 1 ] || fail "nproc returned $test_threads"
    [ "$test_threads" -le 4 ] || test_threads=4
    export CARGO_BUILD_JOBS="$jobs"
    export RUST_TEST_THREADS="$test_threads"
    export CMAKE_BUILD_PARALLEL_LEVEL=1
    export RUSTUP_TOOLCHAIN=1.94.0
    export CC=/opt/clang/bin/clang
    export CXX=/opt/clang/bin/clang++
    unset CARGO_PROFILE_RELEASE_LTO RUSTFLAGS CARGO_TARGET_DIR 2>/dev/null || true

    {
        echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "mode=$MODE"
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
        echo "cargo_home=${CARGO_HOME-}"
        echo "rustup_home=${RUSTUP_HOME-}"
        echo "cargo_build_jobs=${CARGO_BUILD_JOBS-}"
        echo "rust_test_threads=${RUST_TEST_THREADS-}"
        echo "cmake_build_parallel_level=${CMAKE_BUILD_PARALLEL_LEVEL-}"
        echo "rustup_toolchain=${RUSTUP_TOOLCHAIN-}"
        echo "cargo_profile_release_lto=${CARGO_PROFILE_RELEASE_LTO-<unset>}"
        echo "rustflags=${RUSTFLAGS-<unset>}"
        echo "cargo_target_dir=${CARGO_TARGET_DIR-<unset>}"
        echo "cc=${CC-}"
        echo "cxx=${CXX-}"
        echo "rustc_path=$(command -v rustc)"
        echo "cargo_path=$(command -v cargo)"
        rustc -vV
        echo "cargo=$(cargo --version)"
        echo "rustup=$(rustup --version 2>&1 | awk 'NR == 1')"
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
        echo "env_begin"
        env | grep -E '^(CARGO|RUST|CMAKE|CC|CXX|PATH|HOME|LD_LIBRARY_PATH)=' |
            sort || true
        echo "env_end"
    } >"$EVIDENCE/environment.txt"

    assert_eq "$(uname -m)" "aarch64" "container architecture"
    assert_eq "$(getconf GNU_LIBC_VERSION)" "glibc 2.28" "container glibc"
    assert_eq "$(rustc -V | awk '{print $2}')" "1.94.0" "rustc version"
    assert_eq "$(cargo --version | awk '{print $2}')" "1.94.0" "cargo version"
    grep -Fq 'host: aarch64-unknown-linux-gnu' <(rustc -vV) ||
        fail "rustc host is not aarch64-unknown-linux-gnu"
    grep -Fq "$CLANG_VERSION_TEXT" <($CC --version) ||
        fail "unexpected Clang version"
    grep -Fq "libprotoc ${PROTOC_VERSION}" <(protoc --version) ||
        fail "unexpected protoc version"
    assert_eq "$(sed -n 's/^cargo_profile_release_lto=//p' "$EVIDENCE/environment.txt")" \
        "<unset>" "release LTO variable"
    assert_eq "$(sed -n 's/^cargo_target_dir=//p' "$EVIDENCE/environment.txt")" \
        "<unset>" "Cargo target directory override"
}

record_source_identity() {
    local head changed dirty pin_count product_diff workflow_diff
    local product_paths=(
        src include Cargo.toml Cargo.lock build.rs CMakeLists.txt cmake
        .github/scripts examples/cmake-consumer test_data
    )

    head=$(git -C "$SOURCE" rev-parse HEAD)
    assert_eq "$head" "$VALIDATION_COMMIT" "source HEAD"
    git -C "$SOURCE" merge-base --is-ancestor "$PRODUCT_COMMIT" "$head" ||
        fail "product commit is not an ancestor of validation HEAD"

    changed=$(git -C "$SOURCE" diff --name-only "$PRODUCT_COMMIT" "$head")
    if [ -n "$changed" ] &&
        printf '%s\n' "$changed" |
            grep -Ev "^(${VALIDATION_DIR}/|${WORKFLOW_PATH}$)" >/dev/null; then
        fail "validation branch changes files outside the validation directory"
    fi
    product_diff=$(git -C "$SOURCE" diff --name-only \
        "$PRODUCT_COMMIT" "$head" -- "${product_paths[@]}")
    assert_eq "$product_diff" "" "product path diff"
    workflow_diff=$(git -C "$SOURCE" diff --name-only \
        "$PRODUCT_COMMIT" "$head" -- \
        .github/workflows/ci.yml .github/workflows/release.yml)
    assert_eq "$workflow_diff" "" "product workflow diff"

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
        echo "product_workflow_diff=$workflow_diff"
        echo "source_status_porcelain=$dirty"
        echo "lance_pin_count=$pin_count"
        echo "validation_script_sha256=$(sha256sum "$SCRIPT_PATH" | awk '{print $1}')"
        echo "product_tree_objects_begin"
        git -C "$SOURCE" ls-tree -r "$PRODUCT_COMMIT"
        echo "product_tree_objects_end"
        echo "validation_tree_objects_begin"
        git -C "$SOURCE" ls-tree -r "$head" -- "$VALIDATION_DIR" "$WORKFLOW_PATH"
        echo "validation_tree_objects_end"
        echo "header_sha256=$(sha256sum "$SOURCE/include/lance/lance.h" |
            awk '{print $1}')"
        echo "lock_sha256=$(sha256sum "$SOURCE/Cargo.lock" | awk '{print $1}')"
        echo "manifest_sha256=$(sha256sum "$SOURCE/Cargo.toml" |
            awk '{print $1}')"
    } >"$EVIDENCE/source-identity.txt"
}

configure_release() {
    cmake -S "$SOURCE" -B "$WORK/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_LIBDIR=lib
}

verify_configure_toolchain() {
    local log="$LOG_DIR/configure-release.log"
    grep -Fq 'The C compiler identification is Clang 20.1.8' "$log" ||
        fail "CMake did not select Clang 20.1.8 for C"
    grep -Fq 'The CXX compiler identification is Clang 20.1.8' "$log" ||
        fail "CMake did not select Clang 20.1.8 for C++"
    grep -Fq 'Rust Toolchain: 1.94.0-aarch64-unknown-linux-gnu' "$log" ||
        fail "CMake did not select Rust 1.94.0 for aarch64"
    grep -Fq 'Rust Target: aarch64-unknown-linux-gnu' "$log" ||
        fail "CMake did not select the aarch64 Rust target"
    {
        echo "cmake_c_compiler=$(sed -n \
            's/^CMAKE_C_COMPILER:[^=]*=//p' "$WORK/build/CMakeCache.txt" | tail -1)"
        echo "cmake_cxx_compiler=$(sed -n \
            's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$WORK/build/CMakeCache.txt" | tail -1)"
        echo "cmake_install_libdir=$(sed -n \
            's/^CMAKE_INSTALL_LIBDIR:[^=]*=//p' "$WORK/build/CMakeCache.txt" | tail -1)"
        grep -E 'Rust Toolchain|Rust Target|C compiler identification|CXX compiler identification' \
            "$log" || true
    } >"$EVIDENCE/configure-toolchain.txt"
    assert_eq "$(sed -n 's/^cmake_install_libdir=//p' \
        "$EVIDENCE/configure-toolchain.txt")" "lib" "CMake install libdir"
}

build_release() {
    cmake --build "$WORK/build" --parallel 1
    du -sh "$WORK/build" >"$EVIDENCE/build-size.txt"
    grep -F 'Finished `release` profile' "$LOG_DIR/build-release.log" \
        >"$EVIDENCE/cargo-finish.txt" ||
        fail "Cargo did not report a finished release profile"
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
    path_tree "$prefix" "$EVIDENCE/${label}.paths"
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
    readelf -S "$WORK/stage/lib/liblance_c.so" \
        >"$EVIDENCE/readelf-sections.txt"
    if grep -q '\.symtab' "$EVIDENCE/readelf-sections.txt"; then
        fail "shared library still contains a .symtab section"
    fi
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
    assert_eq "$after" "$before" "static archive changed during shared strip"
}

verify_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$WORK/stage/lib/liblance_c.so" \
        "$WORK/stage/include/lance/lance.h" linux
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
    assert_eq "$(printf '%s\n' "$actual" | wc -l)" "$EXPECTED_EXPORTS" \
        "defined dynamic export count"
    assert_eq "$(printf '%s\n' "$expected" | wc -l)" "$EXPECTED_EXPORTS" \
        "header declaration count"
    printf '%s\n' "$actual" | grep -v '^lance_' \
        >"$EVIDENCE/non-lance-exports.txt" || true
    [ ! -s "$EVIDENCE/non-lance-exports.txt" ] ||
        fail "shared library exports non-lance symbols"
    cmp -s "$EVIDENCE/header-exports.txt" "$EVIDENCE/shared-exports.txt" ||
        fail "export set differs from the installed header"
    {
        echo "header_declarations=$(wc -l <"$EVIDENCE/header-exports.txt")"
        echo "defined_dynamic_exports=$(wc -l <"$EVIDENCE/shared-exports.txt")"
        echo "expected_export_count=$EXPECTED_EXPORTS"
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

    grep -q 'Machine:.*AArch64' "$EVIDENCE/$label-readelf-header.txt" ||
        fail "ELF machine is not AArch64"
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
    local static_reads shared_reads passes clone_lines error_lines scan_lines create_lines

    assert_file "$log"
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
    create_lines=$(grep -c 'acceptance: create rows=4 version=1' "$log" || true)

    assert_eq "$static_reads" "1" "static historical dataset reads"
    assert_eq "$shared_reads" "1" "shared historical dataset reads"
    assert_eq "$passes" "2" "acceptance PASS lines"
    assert_eq "$clone_lines" "2" "clone-lifetime acceptance lines"
    assert_eq "$error_lines" "2" "last-error acceptance lines"
    assert_eq "$create_lines" "2" "create acceptance lines"
    [ "$scan_lines" -ge 4 ] ||
        fail "expected at least four scan acceptance lines, found $scan_lines"
}

verify_one_consumer_log() {
    local log=$1
    local linkage=$2
    local reads other_reads passes clone_lines error_lines scan_lines create_lines

    assert_file "$log"
    reads=$(grep -c \
        "Verified ${linkage} consumer dataset read: rows: 512, version: 2" \
        "$log" || true)
    if [ "$linkage" = static ]; then
        other_reads=$(grep -c \
            'Verified shared consumer dataset read:' "$log" || true)
    else
        other_reads=$(grep -c \
            'Verified static consumer dataset read:' "$log" || true)
    fi
    passes=$(grep -c 'acceptance: PASS' "$log" || true)
    clone_lines=$(grep -c \
        'acceptance: clone rows=4 source_closed_before_use=yes' "$log" || true)
    error_lines=$(grep -c \
        'acceptance: last_error code=3 message_bytes=' "$log" || true)
    scan_lines=$(grep -c \
        'acceptance: scan rows=4 batches=2 array_releases=2 schema_releases=1 stream_releases=1' \
        "$log" || true)
    create_lines=$(grep -c 'acceptance: create rows=4 version=1' "$log" || true)

    assert_eq "$reads" "1" "$linkage historical dataset reads"
    assert_eq "$other_reads" "0" "unexpected alternate linkage reads"
    assert_eq "$passes" "1" "$linkage acceptance PASS lines"
    assert_eq "$clone_lines" "1" "$linkage clone-lifetime acceptance lines"
    assert_eq "$error_lines" "1" "$linkage last-error acceptance lines"
    assert_eq "$create_lines" "1" "$linkage create acceptance lines"
    [ "$scan_lines" -ge 2 ] ||
        fail "expected at least two $linkage scan acceptance lines"
}

archive_version() {
    sed -n 's/^version = "\([^"]*\)"/\1/p' "$SOURCE/Cargo.toml" | head -1
}

make_archive() {
    local version archive name
    version=$(archive_version)
    assert_eq "$version" "0.1.9" "workspace version"
    name="lance-c-v${version}-${TARGET}.tar.xz"
    archive="$EVIDENCE/artifacts/$name"
    mkdir -p "$EVIDENCE/artifacts"
    rm -f "$archive" "$archive.sha256" "$archive.sha512"
    (
        cd "$EVIDENCE/artifacts"
        XZ_OPT=-T0 tar -C "$WORK/stage" -cJf "$name" .
        xz -t "$name"
        sha256sum "$name" >"$name.sha256"
        sha512sum "$name" >"$name.sha512"
        sha256sum -c "$name.sha256"
        sha512sum -c "$name.sha512"
    )
    tar -tJf "$archive" >"$EVIDENCE/archive-members.txt"
    tar -tvJf "$archive" >"$EVIDENCE/archive-members-verbose.txt"
    if grep -Eq '^/|(^|/)\.\.(/|$)' "$EVIDENCE/archive-members.txt"; then
        fail "archive contains an absolute or parent-relative path"
    fi
    file "$archive" >"$EVIDENCE/archive-file.txt" 2>&1 || true
    stat -c '%n %s' "$archive" >"$EVIDENCE/archive-size.txt"
    {
        echo "archive=$archive"
        echo "archive_name=$name"
        echo "archive_member_count=$(wc -l <"$EVIDENCE/archive-members.txt")"
        cat "$EVIDENCE/archive-size.txt"
        cat "$archive.sha256"
        cat "$archive.sha512"
    } >"$EVIDENCE/archive-identity.txt"
}

extract_archive() {
    local archive
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    assert_file "$archive"
    [ ! -e "$WORK/extracted" ] || fail "extracted prefix already exists"
    mkdir -p "$WORK/extracted"
    tar -xJf "$archive" -C "$WORK/extracted"
}

verify_extracted_manifest() {
    local paths
    manifest_tree "$WORK/extracted" "$EVIDENCE/extracted-manifest.sha256"
    path_tree "$WORK/extracted" "$EVIDENCE/extracted.paths"
    cmp "$EVIDENCE/stage-manifest.sha256" \
        "$EVIDENCE/extracted-manifest.sha256" ||
        fail "extracted archive manifest differs from installed stage"
    cmp "$EVIDENCE/stage.paths" "$EVIDENCE/extracted.paths" ||
        fail "extracted archive path set differs from installed stage"
    paths=$(find "$WORK/extracted" -type l -print -quit)
    assert_eq "$paths" "" "symlinks in extracted archive"
}

disable_original_prefix() {
    [ -d "$WORK/stage" ] || fail "original install prefix is missing"
    [ ! -e "$WORK/stage-unavailable" ] ||
        fail "original prefix preservation path already exists"
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

verify_metadata_paths() {
    local path_hits build_hits source_hits
    path_hits=$(grep -R -a -l -F "$WORK/stage" "$WORK/extracted" \
        --include='*.cmake' --include='*.pc' --include='*.h' \
        --include='*.hpp' || true)
    assert_eq "$path_hits" "" "extracted metadata references unavailable prefix"
    build_hits=$(grep -R -a -l -F "$WORK/build" "$WORK/extracted" \
        --include='*.cmake' --include='*.pc' --include='*.h' \
        --include='*.hpp' || true)
    assert_eq "$build_hits" "" "extracted metadata references build directory"
    source_hits=$(grep -R -a -l -F "$SOURCE" "$WORK/extracted" \
        --include='*.cmake' --include='*.pc' --include='*.h' \
        --include='*.hpp' || true)
    assert_eq "$source_hits" "" "extracted metadata references source directory"
    {
        echo "original_prefix_path_hits=0"
        echo "build_path_metadata_hits=0"
        echo "source_path_metadata_hits=0"
        echo "note=generated Rust object strings are not package metadata"
    } >"$EVIDENCE/path-audit.txt"
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

extracted_consumer() {
    local linkage=$1
    local build_dir="$WORK/consumers/extracted-$linkage"
    local log="$LOG_DIR/consumer-${linkage}.log"
    local exe="$build_dir/consumer"
    local acceptance="$build_dir/acceptance"
    local dataset_uri="$SOURCE/test_data/v0.27.1/pq_in_schema"
    local acceptance_path="$WORK/datasets/extracted-${linkage}.lance"
    local ld_path
    local dataset_output rc
    local link_txt="$build_dir/CMakeFiles/consumer.dir/link.txt"
    local link_acceptance="$build_dir/CMakeFiles/acceptance.dir/link.txt"

    rm -rf "$build_dir" "$acceptance_path"
    mkdir -p "$build_dir" "$WORK/datasets"

    cmake -S "$SOURCE/examples/cmake-consumer" -B "$build_dir" \
        -DCMAKE_PREFIX_PATH="$WORK/extracted" \
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SKIP_RPATH=ON \
        -DLanceC_LINK="$linkage"
    grep -Fq "LanceC_DIR:PATH=$WORK/extracted/lib/cmake/LanceC" \
        "$build_dir/CMakeCache.txt" ||
        fail "$linkage consumer did not resolve LanceC from extracted prefix"
    cmake --build "$build_dir" --parallel 1

    assert_file "$link_txt"
    assert_file "$link_acceptance"
    if grep -Fq "$WORK/stage" "$link_txt" "$link_acceptance"; then
        fail "$linkage consumer link command references unavailable prefix"
    fi
    if [ "$linkage" = static ]; then
        grep -Fq "$WORK/extracted/lib/liblance_c.a" "$link_txt" ||
            fail "static consumer does not link the extracted static archive"
        ld_path=(env -u LD_LIBRARY_PATH)
    else
        grep -Fq "$WORK/extracted/lib/liblance_c.so" "$link_txt" ||
            grep -Fq -- "-L$WORK/extracted/lib" "$link_txt" ||
            fail "shared consumer does not link from extracted prefix"
        ld_path=(env "LD_LIBRARY_PATH=$WORK/extracted/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
    fi

    printf '==> extracted %s find_package configure/build\n' "$linkage"
    printf '==> extracted %s usage check\n' "$linkage"
    set +e
    "${ld_path[@]}" "$exe" >>"$log" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" "2" "$linkage consumer usage exit code"

    printf '==> extracted %s dataset check\n' "$linkage"
    set +e
    dataset_output=$("${ld_path[@]}" "$exe" "$dataset_uri" 2>&1)
    rc=$?
    set -e
    printf '%s\n' "$dataset_output" >>"$log"
    assert_eq "$rc" "0" "$linkage consumer dataset exit code"
    assert_eq "$dataset_output" "rows: 512, version: 2" \
        "$linkage consumer dataset output"
    printf 'Verified %s consumer dataset read: %s\n' \
        "$linkage" "$dataset_output" >>"$log"

    if [ "$linkage" = shared ]; then
        printf '==> extracted shared loader trace\n' >>"$log"
        set +e
        "${ld_path[@]}" LD_DEBUG=libs "$exe" "$dataset_uri" \
            >"$EVIDENCE/shared-loader-trace.log" 2>&1
        rc=$?
        set -e
        assert_eq "$rc" "0" "shared loader trace exit code"
        grep -Fq "calling init: $WORK/extracted/lib/liblance_c.so" \
            "$EVIDENCE/shared-loader-trace.log" ||
            fail "loader trace did not initialize the extracted shared library"
        printf 'Verified shared loader initialized %s\n' \
            "$WORK/extracted/lib/liblance_c.so" >>"$log"
    fi

    printf '==> extracted %s acceptance check\n' "$linkage" >>"$log"
    set +e
    "${ld_path[@]}" "$acceptance" "file://$acceptance_path" >>"$log" 2>&1
    rc=$?
    set -e
    assert_eq "$rc" "0" "$linkage acceptance exit code"

    "${ld_path[@]}" ldd "$exe" \
        >"$EVIDENCE/consumer-${linkage}-ldd.txt" 2>&1 || true
    "${ld_path[@]}" ldd "$acceptance" \
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

verify_extracted_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$WORK/extracted/lib/liblance_c.so" \
        "$WORK/extracted/include/lance/lance.h" linux
}

record_archive_audit() {
    local archive a so member_count machine_count
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    a="$WORK/extracted/lib/liblance_c.a"
    so="$WORK/extracted/lib/liblance_c.so"
    ar t "$a" >"$EVIDENCE/archive-static-members.txt"
    member_count=$(wc -l <"$EVIDENCE/archive-static-members.txt")
    [ "$member_count" -gt 0 ] || fail "static archive has zero members"
    readelf -h "$a" >"$EVIDENCE/archive-static-readelf-header.txt" 2>&1 || true
    machine_count=$(grep -c 'Machine:.*AArch64' \
        "$EVIDENCE/archive-static-readelf-header.txt" || true)
    assert_eq "$machine_count" "$member_count" \
        "static archive members that are AArch64 ELF objects"
    if grep -Eq '(^/|/\.\./|^\.\./)' "$EVIDENCE/archive-static-members.txt"; then
        fail "static archive contains an absolute or parent-relative member name"
    fi
    {
        echo "archive_member_count=$member_count"
        echo "archive_machine_count=$machine_count"
        echo "archive_member_architecture=aarch64"
        echo "archive_static_sha256=$(sha256sum "$a" | awk '{print $1}')"
        echo "archive_shared_sha256=$(sha256sum "$so" | awk '{print $1}')"
        echo "archive_static_bytes=$(stat -c %s "$a")"
        echo "archive_shared_bytes=$(stat -c %s "$so")"
        echo "shared_max_glibc=$(cat "$EVIDENCE/extracted-glibc-max.txt")"
        echo "static_consumer_max_glibc=$(cat "$EVIDENCE/consumer-static-glibc-max.txt")"
        echo "shared_consumer_max_glibc=$(cat "$EVIDENCE/consumer-shared-glibc-max.txt")"
        echo "shared_dt_needed_begin"
        cat "$EVIDENCE/extracted-dt-needed.txt"
        echo "shared_dt_needed_end"
        echo "archive_path=$archive"
    } >"$EVIDENCE/archive-audit.txt"
}

cleanup_release_build() {
    local size
    [ -d "$WORK/build" ] || fail "release build directory is missing"
    size=$(du -sh "$WORK/build" | awk '{print $1}')
    {
        echo "removed_path=$WORK/build"
        echo "removed_size=$size"
        echo "reason=release build tree is not part of the installed acceptance"
        echo "retained=logs, install stage manifest, artifact hashes"
    } >"$EVIDENCE/build-cleanup.txt"
    rm -rf "$WORK/build"
    [ ! -e "$WORK/build" ] || fail "release build directory was not removed"
}

verify_source_unchanged() {
    local dirty
    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source changed during validation"
}

write_summary() {
    local static_sha static_bytes shared_before shared_after
    local shared_before_bytes shared_after_bytes archive archive_sha256
    local archive_sha512 archive_bytes member_count exports dt_needed
    local max_glibc static_glibc shared_glibc

    static_sha=$(awk '$2 ~ /liblance_c\.a$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    static_bytes=$(awk '$2 ~ /liblance_c\.a$/ {print $2}' \
        "$EVIDENCE/stage-poststrip.sha256")
    shared_before=$(awk '$2 ~ /liblance_c\.so$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_before_bytes=$(awk '$2 ~ /liblance_c\.so$/ {print $2}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_after=$(awk '$2 ~ /liblance_c\.so$/ {print $1}' \
        "$EVIDENCE/stage-poststrip.sha256")
    shared_after_bytes=$(awk '$2 ~ /liblance_c\.so$/ {print $2}' \
        "$EVIDENCE/stage-poststrip.sha256")
    archive=$(sed -n 's/^archive=//p' "$EVIDENCE/archive-identity.txt")
    archive_sha256=$(awk '{print $1}' "$archive.sha256")
    archive_sha512=$(awk '{print $1}' "$archive.sha512")
    archive_bytes=$(stat -c %s "$archive")
    member_count=$(sed -n 's/^archive_member_count=//p' \
        "$EVIDENCE/archive-audit.txt")
    exports=$(grep '^Verified ' "$LOG_DIR/exports.log" | tail -1)
    dt_needed=$(paste -sd, "$EVIDENCE/extracted-dt-needed.txt")
    max_glibc=$(cat "$EVIDENCE/extracted-glibc-max.txt")
    static_glibc=$(cat "$EVIDENCE/consumer-static-glibc-max.txt")
    shared_glibc=$(cat "$EVIDENCE/consumer-shared-glibc-max.txt")

    {
        echo "status=pass"
        echo "product_commit=$PRODUCT_COMMIT"
        echo "product_tree=$(git -C "$SOURCE" rev-parse "$PRODUCT_COMMIT^{tree}")"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "validation_parent=$(git -C "$SOURCE" rev-parse "$VALIDATION_COMMIT^")"
        echo "validation_script_sha256=$(sha256sum "$SCRIPT_PATH" | awk '{print $1}')"
        echo "image=$IMAGE_REF"
        echo "image_digest=$IMAGE_DIGEST"
        echo "image_id=$(sed -n 's/^image_id=//p' \
            "$EVIDENCE/orchestration-identity.txt")"
        echo "image_architecture=$(sed -n 's/^image_architecture=//p' \
            "$EVIDENCE/orchestration-identity.txt")"
        echo "container_hostname=$(uname -n)"
        echo "container_arch=$(uname -m)"
        echo "container_libc=$(getconf GNU_LIBC_VERSION)"
        echo "container_os=$(grep '^PRETTY_NAME=' /etc/os-release |
            cut -d= -f2- | tr -d '"')"
        echo "rustc=$(rustc -V)"
        echo "cargo=$(cargo --version)"
        echo "rustup_toolchain=${RUSTUP_TOOLCHAIN-}"
        echo "rust_target=aarch64-unknown-linux-gnu"
        echo "clang=$($CC --version | awk 'NR == 1')"
        echo "cmake=$(cmake --version | awk 'NR == 1')"
        echo "protoc=$(protoc --version)"
        echo "cargo_build_jobs=${CARGO_BUILD_JOBS-}"
        echo "rust_test_threads=${RUST_TEST_THREADS-}"
        echo "cmake_build_parallel_level=${CMAKE_BUILD_PARALLEL_LEVEL-}"
        echo "cargo_profile_release_lto=${CARGO_PROFILE_RELEASE_LTO-<unset>}"
        echo "cargo_target_dir=${CARGO_TARGET_DIR-<unset>}"
        echo "cmake_install_libdir=lib"
        echo "installed_libdir=lib"
        echo "installed_lib_is_symlink=no"
        echo "installed_lib64_exists=no"
        echo "installed_symlink_count=0"
        echo "static_sha256=$static_sha"
        echo "static_bytes=$static_bytes"
        echo "shared_unstripped_sha256=$shared_before"
        echo "shared_unstripped_bytes=$shared_before_bytes"
        echo "shared_stripped_sha256=$shared_after"
        echo "shared_stripped_bytes=$shared_after_bytes"
        echo "exports=$exports"
        echo "header_declarations=$(sed -n 's/^header_declarations=//p' \
            "$EVIDENCE/export-count.txt")"
        echo "defined_dynamic_exports=$(sed -n \
            's/^defined_dynamic_exports=//p' "$EVIDENCE/export-count.txt")"
        echo "elf_machine=$(sed -n 's/.*Machine:[[:space:]]*//p' \
            "$EVIDENCE/extracted-readelf-header.txt")"
        echo "shared_max_glibc=$max_glibc"
        echo "static_consumer_max_glibc=$static_glibc"
        echo "shared_consumer_max_glibc=$shared_glibc"
        echo "shared_dt_needed=$dt_needed"
        echo "cxx17_headers=pass"
        echo "consumer_static_dataset=rows:512,version:2"
        echo "consumer_shared_dataset=rows:512,version:2"
        echo "consumer_acceptance=PASS"
        echo "consumer_clone_after_source_close=yes"
        echo "consumer_last_error_ownership=consumed"
        echo "consumer_arrow_release_counts=2/1/1"
        echo "archive=$archive"
        echo "archive_name=$(basename "$archive")"
        echo "archive_sha256=$archive_sha256"
        echo "archive_sha512=$archive_sha512"
        echo "archive_bytes=$archive_bytes"
        echo "archive_member_count=$member_count"
        echo "archive_member_architecture=aarch64"
        echo "extracted_prefix=$WORK/extracted"
        echo "original_prefix_state=unavailable"
        echo "extracted_static_dataset=rows:512,version:2"
        echo "extracted_shared_dataset=rows:512,version:2"
        echo "extracted_acceptance=PASS"
        echo "shared_runtime_binding=$WORK/extracted/lib/liblance_c.so"
        echo "static_no_liblance_so_dependency=yes"
        echo "original_prefix_path_hits=0"
        echo "build_path_metadata_hits=0"
        echo "published_release=no"
        echo "scope=current-source-candidate"
        echo "source_unchanged=yes"
    } >"$EVIDENCE/summary.env"
}

seal_evidence() {
    [ -d "$EVIDENCE" ] || return 0
    (
        cd "$EVIDENCE"
        find . -type f ! -name 'EVIDENCE-MANIFEST.sha256' -print0 |
            sort -z | xargs -0 sha256sum
    ) >"$EVIDENCE/EVIDENCE-MANIFEST.sha256"
}

record_runner_identity() {
    local runner_label=${RUNNER_LABEL:-unknown}
    local runner_arch=${RUNNER_ARCH:-unknown}
    local runner_os=${RUNNER_OS:-unknown}
    {
        echo "runner_label=$runner_label"
        echo "runner_arch=$runner_arch"
        echo "runner_os=$runner_os"
        echo "runner_environment=${RUNNER_ENVIRONMENT-}"
        echo "image_os=${ImageOS-}"
        echo "host_hostname=$(uname -n)"
        echo "host_uname=$(uname -a)"
        echo "host_arch=$(uname -m)"
        echo "host_nproc=$(nproc)"
        echo "host_os_release_begin"
        cat /etc/os-release
        echo "host_os_release_end"
        echo "docker_version_begin"
        docker version
        echo "docker_version_end"
    } >"$EVIDENCE/host-runner-identity.txt"
}

container_body() {
    [ -n "$SOURCE" ] && [ -n "$WORK" ] && [ -n "$EVIDENCE" ] &&
        [ -n "$PRODUCT_COMMIT" ] && [ -n "$VALIDATION_COMMIT" ] ||
        fail "container body arguments are incomplete"
    SOURCE=$(cd "$SOURCE" && pwd)
    WORK=$(cd "$WORK" && pwd)
    EVIDENCE=$(cd "$EVIDENCE" && pwd)

    export CARGO_HOME=/work/toolchain/cargo
    export RUSTUP_HOME=/work/toolchain/rustup
    export PATH="/work/toolchain/cargo/bin:/opt/clang/bin:/usr/local/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin"
    export RUST_TEST_THREADS=4
    export CMAKE_BUILD_PARALLEL_LEVEL=1
    export RUSTUP_TOOLCHAIN=1.94.0
    unset CARGO_BUILD_JOBS CARGO_PROFILE_RELEASE_LTO RUSTFLAGS CARGO_TARGET_DIR 2>/dev/null || true

    setup_status
    record_environment
    record_source_identity

    run_step configure-release configure_release
    run_step verify-configure-toolchain verify_configure_toolchain
    run_step build-release build_release
    run_step install-release install_release
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
    run_step cleanup-release-build cleanup_release_build
    run_step extract-archive extract_archive
    run_step extracted-layout record_install_layout "$WORK/extracted" extracted
    run_step extracted-manifest verify_extracted_manifest
    run_step disable-original-prefix disable_original_prefix
    run_step path-audit verify_metadata_paths
    run_step consumer-static extracted_consumer static
    verify_one_consumer_log "$LOG_DIR/consumer-static.log" static
    run_step consumer-shared extracted_consumer shared
    verify_one_consumer_log "$LOG_DIR/consumer-shared.log" shared
    run_step linux-staging-extracted verify_linux_staging "$WORK/extracted"
    run_step extracted-exports verify_extracted_exports
    run_step elf-extracted elf_metadata "$WORK/extracted" extracted
    run_step archive-audit record_archive_audit
    run_step source-unchanged verify_source_unchanged
    run_step summary write_summary
    append_status ACCEPTED "manylinux_2_28 aarch64 current-source candidate complete"
}

run_outer() {
    local image_id repo_digests image_arch
    local setup_rc body_rc

    [ -n "$SOURCE" ] && [ -n "$ROOT" ] && [ -n "$PRODUCT_COMMIT" ] ||
        fail "outer arguments are incomplete"
    SOURCE=$(cd "$SOURCE" && pwd)
    ROOT=$(mkdir -p "$ROOT" && cd "$ROOT" && pwd)
    EVIDENCE="$ROOT/evidence"
    WORK="$ROOT/work"
    mkdir -p "$EVIDENCE" "$WORK"
    [ ! -e "$EVIDENCE/commands-status.txt" ] ||
        fail "evidence already contains a completed container run"
    [ ! -e "$EVIDENCE/orchestration-status.txt" ] ||
        fail "evidence already contains an orchestration record"
    : >"$EVIDENCE/orchestration-status.txt"
    LOG_DIR="$EVIDENCE"
    VALIDATION_COMMIT=$(git -C "$SOURCE" rev-parse HEAD)
    CONTAINER_NAME="lance103-arm64-$(date -u +%Y%m%dT%H%M%SZ)-$$"

    assert_eq "$(uname -m)" "aarch64" "host architecture"
    command -v docker >/dev/null || fail "docker is unavailable"
    record_runner_identity
    docker version >"$EVIDENCE/docker-version.txt"
    docker pull "$IMAGE_REF" 2>&1 | tee "$EVIDENCE/docker-pull.log"
    image_id=$(docker image inspect --format '{{.Id}}' "$IMAGE_REF")
    repo_digests=$(docker image inspect \
        --format '{{range .RepoDigests}}{{println .}}{{end}}' "$IMAGE_REF")
    image_arch=$(docker image inspect \
        --format '{{.Os}}/{{.Architecture}}' "$IMAGE_REF")
    assert_eq "$image_arch" "linux/arm64" "container image architecture"
    {
        echo "image=$IMAGE_REF"
        echo "image_id=$image_id"
        echo "image_architecture=$image_arch"
        echo "image_repo_digests=$repo_digests"
        echo "host_arch=$(uname -m)"
        echo "product_commit=$PRODUCT_COMMIT"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "clang_version=$CLANG_VERSION"
        echo "protoc_version=$PROTOC_VERSION"
        echo "protoc_sha256=$PROTOC_SHA256"
        echo "rustup_sha256=$RUSTUP_SHA256"
    } >"$EVIDENCE/orchestration-identity.txt"

    echo "START $(date -u +%Y-%m-%dT%H:%M:%SZ) docker-create" \
        >>"$EVIDENCE/orchestration-status.txt"
    docker run -d --name "$CONTAINER_NAME" --entrypoint /bin/bash \
        -e GIT_OPTIONAL_LOCKS=0 \
        -v "$SOURCE:/source:ro" \
        -v "$WORK:/work" \
        -v "$EVIDENCE:/evidence" \
        -v "$SCRIPT_PATH:/driver:ro" \
        "$IMAGE_REF" \
        -lc 'trap "exit 0" TERM INT; sleep infinity & wait' \
        >>"$EVIDENCE/orchestration-status.txt"
    echo "PASS docker-create" >>"$EVIDENCE/orchestration-status.txt"
    docker inspect "$CONTAINER_NAME" >"$EVIDENCE/container-inspect.json"
    {
        echo "container_name=$CONTAINER_NAME"
        echo "source_mount=$SOURCE:/source:ro"
        echo "work_mount=$WORK:/work"
        echo "evidence_mount=$EVIDENCE:/evidence"
        echo "driver_mount=$SCRIPT_PATH:/driver:ro"
        echo "normal_target_dir=unset"
        echo "normal_cargo_home=/work/toolchain/cargo"
        echo "normal_rustup_home=/work/toolchain/rustup"
    } >"$EVIDENCE/orchestration.txt"

    set +e
    docker exec -u 0 "$CONTAINER_NAME" /bin/bash /driver \
        --container-setup --work /work --evidence /evidence \
        2>&1 | tee "$EVIDENCE/container-setup.log"
    setup_rc=${PIPESTATUS[0]}
    set -e
    docker logs "$CONTAINER_NAME" >"$EVIDENCE/container.log" 2>&1 || true
    docker inspect "$CONTAINER_NAME" >"$EVIDENCE/container-inspect.json" || true
    if [ "$setup_rc" -ne 0 ]; then
        echo "FAIL container-setup rc=$setup_rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        echo "container setup failed with rc=$setup_rc" >"$EVIDENCE/FAILURE.txt"
        seal_evidence
        exit "$setup_rc"
    fi
    echo "PASS container-setup" >>"$EVIDENCE/orchestration-status.txt"

    set +e
    docker exec -u 0 "$CONTAINER_NAME" /bin/bash /driver \
        --container-body \
        --source /source \
        --work /work \
        --evidence /evidence \
        --product-commit "$PRODUCT_COMMIT" \
        --validation-commit "$VALIDATION_COMMIT" \
        2>&1 | tee "$EVIDENCE/container-run.log"
    body_rc=${PIPESTATUS[0]}
    set -e
    docker logs "$CONTAINER_NAME" >"$EVIDENCE/container.log" 2>&1 || true
    docker inspect "$CONTAINER_NAME" >"$EVIDENCE/container-inspect.json" || true
    docker exec -u 0 "$CONTAINER_NAME" chown -R "$(id -u):$(id -g)" \
        /work /evidence >/dev/null 2>&1 || true

    if [ "$body_rc" -ne 0 ]; then
        echo "FAIL container-body rc=$body_rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        echo "container body failed with rc=$body_rc" >"$EVIDENCE/FAILURE.txt"
        seal_evidence
        exit "$body_rc"
    fi
    echo "PASS container-body" >>"$EVIDENCE/orchestration-status.txt"
    echo "DONE $(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >>"$EVIDENCE/orchestration-status.txt"
    docker stop "$CONTAINER_NAME" >/dev/null
    docker rm "$CONTAINER_NAME" >/dev/null
    seal_evidence
}

case "$MODE" in
    container-setup)
        [ -n "$WORK" ] && [ -n "$EVIDENCE" ] ||
            fail "container setup arguments are incomplete"
        WORK=$(cd "$WORK" && pwd)
        EVIDENCE=$(cd "$EVIDENCE" && pwd)
        container_setup
        ;;
    container-body)
        container_body
        ;;
    outer)
        run_outer
        ;;
esac
