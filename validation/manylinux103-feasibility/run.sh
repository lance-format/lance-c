#!/usr/bin/env bash
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
IMAGE=quay.io/pypa/manylinux_2_28_x86_64
IMAGE_DIGEST=sha256:9555ba6c479fa69a2aa80ad1dc57154a60d562b19df159361d9379a02beb1e36
CLANG_VERSION=v20.1.8.0
PROTOC_VERSION=25.3
PROTOC_SHA256=f853e691868d0557425ea290bf7ba6384eef2fa9b04c323afab49a770ba9da80

usage() {
    cat <<'EOF'
usage:
  run.sh --source <git-root> --root <evidence-root> --product-commit <sha>
  run.sh --container-setup --evidence <dir>
  run.sh --container-body --source <dir> --work <dir> --evidence <dir> \
    --product-commit <sha> --validation-commit <sha>
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

version_le() {
    [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]
}

container_setup() {
    local tools=/opt/clang/bin
    local zip=/tmp/protoc-${PROTOC_VERSION}-linux-x86_64.zip

    [ "$(id -u)" = 0 ] || fail "container setup must run as root"
    mkdir -p "$EVIDENCE" /work /home/xuanwo

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

    curl -fsSL --retry 3 -o "$zip" \
        "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-x86_64.zip"
    echo "$PROTOC_SHA256  $zip" | sha256sum -c -
    unzip -q -o "$zip" -d /usr/local
    rm -f "$zip"

    {
        echo "clang=$($tools/clang --version | awk 'NR == 1')"
        echo "clangxx=$($tools/clang++ --version | awk 'NR == 1')"
        echo "protoc=$(protoc --version)"
        echo "protoc_path=$(command -v protoc)"
    } >>"$EVIDENCE/setup-identity.txt"

    chown -R 1000:1000 /work "$EVIDENCE"
}

record_environment() {
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
        echo "rustup_toolchain=${RUSTUP_TOOLCHAIN-}"
        echo "cargo_build_jobs=${CARGO_BUILD_JOBS-}"
        echo "rust_test_threads=${RUST_TEST_THREADS-}"
        echo "cmake_build_parallel_level=${CMAKE_BUILD_PARALLEL_LEVEL-}"
        echo "cargo_profile_release_lto=${CARGO_PROFILE_RELEASE_LTO-}"
        echo "rustflags=${RUSTFLAGS-}"
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
        echo "mbx=$(mbx --version 2>&1 | awk 'NR == 1')"
        echo "mbx_doctor_begin"
        mbx doctor
        echo "mbx_doctor_end"
        echo "mbx_settings_begin"
        mbx settings ls
        echo "mbx_settings_end"
        echo "env_begin"
        env | grep -E '^(CARGO|RUST|CMAKE|CC|CXX|MBX|PATH|HOME|LD_LIBRARY_PATH)=' |
            sort || true
        echo "env_end"
    } >"$EVIDENCE/environment.txt"
}

record_source_identity() {
    local head diff_names dirty pin_count

    head=$(git -C "$SOURCE" rev-parse HEAD)
    assert_eq "$head" "$VALIDATION_COMMIT" "source HEAD"
    git -C "$SOURCE" merge-base --is-ancestor "$PRODUCT_COMMIT" "$head" ||
        fail "product commit is not an ancestor of validation HEAD"

    diff_names=$(git -C "$SOURCE" diff --name-only \
        "$PRODUCT_COMMIT" "$head")
    if [ -n "$diff_names" ] &&
        printf '%s\n' "$diff_names" |
            grep -Ev '^validation/manylinux103-feasibility/' >/dev/null; then
        fail "validation branch changes product files"
    fi

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
        echo "validation_subject=$(git -C "$SOURCE" log -1 --pretty=%s "$head")"
        echo "validation_diff_paths_begin"
        printf '%s\n' "$diff_names"
        echo "validation_diff_paths_end"
        echo "source_status_porcelain=$dirty"
        echo "lance_pin_count=$pin_count"
        echo "validation_script_sha256=$(sha256sum "$SCRIPT_PATH" | awk '{print $1}')"
        echo "input_tree_begin"
        git -C "$SOURCE" ls-tree "$PRODUCT_COMMIT" -- \
            src include Cargo.lock Cargo.toml build.rs CMakeLists.txt \
            .github/scripts/verify-linux-staging.sh \
            .github/scripts/verify-shared-exports.sh \
            .github/scripts/strip-release-shared.sh \
            .github/scripts/verify-cmake-consumer.sh \
            examples/cmake-consumer test_data/v0.27.1/pq_in_schema
        echo "input_tree_end"
        echo "header_sha256=$(sha256sum "$SOURCE/include/lance/lance.h" |
            awk '{print $1}')"
        echo "lock_sha256=$(sha256sum "$SOURCE/Cargo.lock" | awk '{print $1}')"
        echo "manifest_sha256=$(sha256sum "$SOURCE/Cargo.toml" |
            awk '{print $1}')"
    } >"$EVIDENCE/source-identity.txt"
}

setup_status() {
    STATUS_FILE="$EVIDENCE/commands-status.txt"
    [ ! -e "$STATUS_FILE" ] ||
        fail "evidence directory already contains a run: $EVIDENCE"
    : >"$STATUS_FILE"
    LOG_DIR="$EVIDENCE/logs"
    mkdir -p "$LOG_DIR"
    trap 'rc=$?; printf "SCRIPT_EXIT rc=%d at %s\n" "$rc" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$STATUS_FILE"; exit "$rc"' EXIT
}

run_step() {
    local name=$1
    shift
    local log="$LOG_DIR/$name.log"
    local rc

    printf 'START %s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$name" \
        >>"$STATUS_FILE"
    printf '$ %s\n' "$*" >"$log"
    set +e
    "$@" >>"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL %s rc=%d log=%s\n' "$name" "$rc" "$log" \
            >>"$STATUS_FILE"
        return "$rc"
    fi
    printf 'PASS %s\n' "$name" >>"$STATUS_FILE"
}

configure_release() {
    cmake -S "$SOURCE" -B "$WORK/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
}

build_release() {
    cmake --build "$WORK/build" --parallel 1
}

install_release() {
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

record_export_sets() {
    local library="$WORK/stage/lib/liblance_c.so"
    local header="$WORK/stage/include/lance/lance.h"
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
    bash "$SOURCE/.github/scripts/verify-linux-staging.sh" \
        "$WORK/stage" x86_64-unknown-linux-gnu
}

record_elf_metadata() {
    local library="$WORK/stage/lib/liblance_c.so"
    local max_glibc

    readelf -h "$library" >"$EVIDENCE/readelf-header.txt"
    readelf -d "$library" >"$EVIDENCE/readelf-dynamic.txt"
    readelf -l "$library" >"$EVIDENCE/readelf-program-headers.txt"
    readelf --version-info "$library" >"$EVIDENCE/readelf-version-info.txt"
    ldd "$library" >"$EVIDENCE/ldd.txt" 2>&1 || true
    file "$library" >"$EVIDENCE/file-shared.txt" 2>&1 || \
        echo "file=unavailable" >"$EVIDENCE/file-shared.txt"

    grep -q 'Machine:.*X86-64' "$EVIDENCE/readelf-header.txt" ||
        fail "ELF machine is not X86-64"
    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/readelf-version-info.txt" | sort -uV \
        >"$EVIDENCE/glibc-versions.txt"
    [ -s "$EVIDENCE/glibc-versions.txt" ] ||
        fail "no GLIBC symbol versions found"
    max_glibc=$(tail -1 "$EVIDENCE/glibc-versions.txt" |
        sed 's/^GLIBC_//')
    echo "$max_glibc" >"$EVIDENCE/glibc-max.txt"
    version_le "$max_glibc" "2.28" ||
        fail "shared library requires GLIBC_$max_glibc, above 2.28"

    grep -oE 'GCC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/readelf-version-info.txt" | sort -uV \
        >"$EVIDENCE/gcc-versions.txt" || true
    if grep -Eq 'GLIBCXX_|CXXABI_' "$EVIDENCE/readelf-version-info.txt"; then
        fail "shared library references a C++ ABI symbol version"
    fi
}

verify_cxx17_headers() {
    "$CXX" -std=c++17 -fsyntax-only \
        -I"$WORK/stage/include" \
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

verify_source_unchanged() {
    local dirty
    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source changed during validation"
}

write_summary() {
    local static_sha static_bytes shared_before shared_before_bytes
    local shared_after shared_after_bytes dt_needed export_line

    static_sha=$(sha256sum "$WORK/stage/lib/liblance_c.a" | awk '{print $1}')
    static_bytes=$(awk '$1 ~ /liblance_c\.a$/ {print $2}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_before=$(awk '$2 ~ /liblance_c\.so$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_before_bytes=$(awk '$1 ~ /liblance_c\.so$/ {print $2}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_after=$(sha256sum "$WORK/stage/lib/liblance_c.so" |
        awk '{print $1}')
    shared_after_bytes=$(stat -c %s "$WORK/stage/lib/liblance_c.so")
    export_line=$(grep '^Verified ' "$LOG_DIR/exports.log" | tail -1)
    dt_needed=$(sed -n \
        's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' \
        "$EVIDENCE/readelf-dynamic.txt" | paste -sd, -)

    {
        echo "status=pass"
        echo "product_commit=$PRODUCT_COMMIT"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "image=$IMAGE@$IMAGE_DIGEST"
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
        echo "static_sha256=$static_sha"
        echo "static_bytes=$static_bytes"
        echo "shared_unstripped_sha256=$shared_before"
        echo "shared_unstripped_bytes=$shared_before_bytes"
        echo "shared_stripped_sha256=$shared_after"
        echo "shared_stripped_bytes=$shared_after_bytes"
        echo "exports=$export_line"
        echo "header_declarations=$(sed -n 's/^header_declarations=//p' \
            "$EVIDENCE/export-count.txt")"
        echo "defined_dynamic_exports=$(sed -n \
            's/^defined_dynamic_exports=//p' "$EVIDENCE/export-count.txt")"
        echo "elf_machine=$(sed -n 's/.*Machine:[[:space:]]*//p' \
            "$EVIDENCE/readelf-header.txt")"
        echo "max_glibc=$(cat "$EVIDENCE/glibc-max.txt")"
        echo "gcc_symbol_versions=$(paste -sd, \
            "$EVIDENCE/gcc-versions.txt")"
        echo "dt_needed=$dt_needed"
        echo "cxx17_headers=pass"
        echo "consumer_static_dataset=rows:512,version:2"
        echo "consumer_shared_dataset=rows:512,version:2"
        echo "consumer_acceptance=PASS"
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
    unset CARGO_PROFILE_RELEASE_LTO RUSTFLAGS 2>/dev/null || true

    setup_status
    record_environment
    record_source_identity
    assert_eq "$(rustc -V | awk '{print $2}')" "1.94.0" "rustc version"
    assert_eq "$(cargo --version | awk '{print $2}')" "1.94.0" \
        "cargo version"
    assert_eq "$(getconf GNU_LIBC_VERSION)" "glibc 2.28" \
        "container glibc"

    run_step configure-release configure_release
    run_step build-release build_release
    run_step install-release install_release
    run_step record-artifacts record_artifacts
    run_step stage-prestrip record_stage_prestrip
    run_step strip-shared strip_shared
    run_step static-untouched verify_static_untouched
    sha256sum "$WORK/stage/lib/liblance_c.so" \
        >"$EVIDENCE/stage-poststrip.sha256"
    run_step exports verify_exports
    run_step export-sets record_export_sets
    run_step installed-headers verify_installed_headers
    run_step linux-staging verify_linux_staging
    run_step elf-metadata record_elf_metadata
    run_step cxx17-headers verify_cxx17_headers
    run_step consumer-installed run_installed_consumer
    verify_consumer_log "$LOG_DIR/consumer-installed.log"
    run_step source-unchanged verify_source_unchanged
    run_step summary write_summary
}

run_outer() {
    local image_ref="$IMAGE@$IMAGE_DIGEST"
    local image_id repo_digests
    local container_name="lance103-manylinux-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    local rc=0

    SOURCE=$(cd "$SOURCE" && pwd)
    ROOT=$(mkdir -p "$ROOT" && cd "$ROOT" && pwd)
    EVIDENCE="$ROOT/evidence"
    WORK="$ROOT/work"
    mkdir -p "$EVIDENCE" "$WORK"
    [ ! -e "$EVIDENCE/commands-status.txt" ] ||
        fail "evidence already exists: $EVIDENCE"
    : >"$EVIDENCE/orchestration-status.txt"
    LOG_DIR="$EVIDENCE/logs"
    mkdir -p "$LOG_DIR"
    VALIDATION_COMMIT=$(git -C "$SOURCE" rev-parse HEAD)

    [ "$(uname -m)" = x86_64 ] || fail "NieR host is not x86_64"
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

    printf 'START %s docker-create\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >>"$EVIDENCE/orchestration-status.txt"
    docker run -d --name "$container_name" \
        -e HOME=/home/xuanwo \
        -e CARGO_HOME=/home/xuanwo/.cargo \
        -e RUSTUP_HOME=/home/xuanwo/.rustup \
        -e XDG_CACHE_HOME=/home/xuanwo/.cache \
        -e XDG_CONFIG_HOME=/home/xuanwo/.config \
        -e GIT_OPTIONAL_LOCKS=0 \
        -e PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.cargo/bin:/opt/clang/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
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
    printf 'PASS docker-create\n' >>"$EVIDENCE/orchestration-status.txt"

    {
        echo "docker_create=pass"
        echo "container_name=$container_name"
        echo "source_mount=$SOURCE:/source:ro"
        echo "work_mount=$WORK:/work"
        echo "evidence_mount=$EVIDENCE:/evidence"
        echo "driver_mount=$SCRIPT_PATH:/driver:ro"
        echo "mbx_cache_mount=/home/xuanwo/.cache/mbx"
        echo "rustup_mount=/home/xuanwo/.rustup:ro"
        echo "cargo_home_mount=/home/xuanwo/.cargo:rw"
    } >"$EVIDENCE/orchestration.txt"

    set +e
    docker exec -u 0 "$container_name" bash /driver \
        --container-setup --evidence /evidence \
        2>&1 | tee "$EVIDENCE/container-setup.log"
    rc=${PIPESTATUS[0]}
    set -e
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL container-setup rc=%d\n' "$rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        docker logs "$container_name" >"$EVIDENCE/container.log" 2>&1 || true
        docker inspect "$container_name" >"$EVIDENCE/container-inspect.json" || true
        echo "container setup failed with rc=$rc; container $container_name retained" \
            >"$EVIDENCE/FAILURE.txt"
        exit "$rc"
    fi
    printf 'PASS container-setup\n' >>"$EVIDENCE/orchestration-status.txt"

    set +e
    docker exec --user 1000:1000 \
        -e HOME=/home/xuanwo \
        -e CARGO_HOME=/home/xuanwo/.cargo \
        -e RUSTUP_HOME=/home/xuanwo/.rustup \
        -e XDG_CACHE_HOME=/home/xuanwo/.cache \
        -e XDG_CONFIG_HOME=/home/xuanwo/.config \
        -e GIT_OPTIONAL_LOCKS=0 \
        -e PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.cargo/bin:/opt/clang/bin:/opt/rh/gcc-toolset-14/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        -e GIT_CONFIG_COUNT=1 \
        -e GIT_CONFIG_KEY_0=safe.directory \
        -e GIT_CONFIG_VALUE_0='*' \
        "$container_name" bash /driver \
        --container-body \
        --source /source \
        --work /work \
        --evidence /evidence \
        --product-commit "$PRODUCT_COMMIT" \
        --validation-commit "$VALIDATION_COMMIT" \
        2>&1 | tee "$EVIDENCE/container-run.log"
    rc=${PIPESTATUS[0]}
    set -e

    docker logs "$container_name" >"$EVIDENCE/container.log" 2>&1 || true
    docker inspect "$container_name" >"$EVIDENCE/container-inspect.json" || true
    docker exec -u 0 "$container_name" chown -R 1000:1000 /evidence /work \
        >/dev/null 2>&1 || true

    if [ "$rc" -ne 0 ]; then
        printf 'FAIL container-body rc=%d\n' "$rc" \
            >>"$EVIDENCE/orchestration-status.txt"
        echo "container body failed with rc=$rc; container $container_name retained" \
            >"$EVIDENCE/FAILURE.txt"
        exit "$rc"
    fi
    printf 'PASS container-body\n' >>"$EVIDENCE/orchestration-status.txt"
    docker stop "$container_name" >/dev/null
    docker rm "$container_name" >/dev/null
    printf 'DONE %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
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
        run_outer
        ;;
esac
