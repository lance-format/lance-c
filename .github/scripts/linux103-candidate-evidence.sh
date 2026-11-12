#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Exercise one current Linux release-candidate row end to end. This is
# temporary issue-103 validation support, not permanent product CI.

set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'EOF'
usage: linux103-candidate-evidence.sh \
  --source <source-root> --work <work-root> \
  --target <x86_64-unknown-linux-gnu|aarch64-unknown-linux-gnu> \
  --version <archive-version> --expected-commit <sha> \
  [--suffix <archive-suffix>] [--validation-commit <sha>] \
  [--container <image>]

With --container, the script pulls the image and re-runs itself inside it.
The container receives the source, work tree and script as bind mounts.
EOF
}

SCRIPT_PATH=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
MODE=outer
SOURCE=
WORK=
TARGET=
SUFFIX=
VERSION=
EXPECTED_COMMIT=
VALIDATION_COMMIT=unknown
CONTAINER_IMAGE=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --container-body)
            MODE=container-body
            shift
            ;;
        --source)
            SOURCE=$2
            shift 2
            ;;
        --work)
            WORK=$2
            shift 2
            ;;
        --target)
            TARGET=$2
            shift 2
            ;;
        --suffix)
            SUFFIX=$2
            shift 2
            ;;
        --version)
            VERSION=$2
            shift 2
            ;;
        --expected-commit)
            EXPECTED_COMMIT=$2
            shift 2
            ;;
        --validation-commit)
            VALIDATION_COMMIT=$2
            shift 2
            ;;
        --container)
            CONTAINER_IMAGE=$2
            shift 2
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$SOURCE" ] || [ -z "$WORK" ] || [ -z "$TARGET" ] ||
    [ -z "$VERSION" ] || [ -z "$EXPECTED_COMMIT" ]; then
    usage >&2
    exit 2
fi

case "$TARGET" in
    x86_64-unknown-linux-gnu | aarch64-unknown-linux-gnu) ;;
    *)
        echo "unsupported Linux target: $TARGET" >&2
        exit 2
        ;;
esac

SOURCE=$(cd "$SOURCE" && pwd)
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)

EVIDENCE="$WORK/evidence"
LOG_DIR="$EVIDENCE/logs"
STATUS_FILE="$EVIDENCE/commands-status.txt"

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

assert_eq() {
    local actual=$1 expected=$2 message=$3
    if [ "$actual" != "$expected" ]; then
        fail "$message: expected '$expected', found '$actual'"
    fi
}

assert_nonempty() {
    local value=$1 message=$2
    if [ -z "$value" ]; then
        fail "$message"
    fi
}

on_exit() {
    local rc=$?
    if [ -f "$STATUS_FILE" ]; then
        printf 'SCRIPT_EXIT rc=%d at %s\n' \
            "$rc" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >>"$STATUS_FILE"
    fi
    exit "$rc"
}

run_step() {
    local name=$1
    shift
    local log="$LOG_DIR/$name.log"
    local rc

    printf 'START %s %s\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$name" >>"$STATUS_FILE"
    printf '$ %s\n' "$*" >"$log"
    set +e
    "$@" >>"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        printf 'FAIL %s rc=%d log=%s\n' "$name" "$rc" "$log" >>"$STATUS_FILE"
        return "$rc"
    fi
    printf 'PASS %s\n' "$name" >>"$STATUS_FILE"
}

setup_debian_container() {
    [ -f /etc/debian_version ] || fail "container is not Debian"
    case "$(cat /etc/debian_version)" in
        12.*) ;;
        *) fail "container is not Debian 12: $(cat /etc/debian_version)" ;;
    esac

    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        git \
        libprotobuf-dev \
        perl \
        pkg-config \
        protobuf-compiler \
        xz-utils \
        zstd

    git config --global --add safe.directory '*'

    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        -o /tmp/rustup-init.sh
    sh /tmp/rustup-init.sh -y --profile minimal \
        --default-toolchain 1.94.0 --no-modify-path
    export PATH="$HOME/.cargo/bin:$PATH"
}

run_container_outer() {
    local inner_work="$WORK/container-work"
    local rc
    local image_id
    local repo_digests

    mkdir -p "$WORK"
    docker pull "$CONTAINER_IMAGE" 2>&1 |
        tee "$WORK/docker-pull.log"
    image_id=$(docker image inspect --format '{{.Id}}' "$CONTAINER_IMAGE")
    repo_digests=$(docker image inspect \
        --format '{{range .RepoDigests}}{{println .}}{{end}}' \
        "$CONTAINER_IMAGE")

    {
        echo "container_image=$CONTAINER_IMAGE"
        echo "container_image_id=$image_id"
        echo "container_repo_digests=$repo_digests"
        echo "docker_server_version=$(docker version --format '{{.Server.Version}}')"
        echo "host_hostname=$(uname -n)"
        echo "host_uname=$(uname -a)"
        echo "host_arch=$(uname -m)"
        echo "host_os_release_begin"
        cat /etc/os-release
        echo "host_os_release_end"
        echo "docker_run_begin"
        echo "docker run --rm -e CC=clang -e CXX=clang++ \\"
        echo "  -e CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-8} \\"
        echo "  -e RUST_TEST_THREADS=${RUST_TEST_THREADS:-4} \\"
        echo "  -e CMAKE_BUILD_PARALLEL_LEVEL=1 \\"
        echo "  -v $SOURCE:/source -v $WORK:/work -v $SCRIPT_PATH:/script:ro \\"
        echo "  $CONTAINER_IMAGE bash /script --container-body ..."
        echo "docker_run_end"
    } >"$WORK/container-orchestration.txt"

    rm -rf "$inner_work"
    mkdir -p "$inner_work"

    set +e
    docker run --rm \
        --name "lance103-$$" \
        -e CC=clang \
        -e CXX=clang++ \
        -e "CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-8}" \
        -e "RUST_TEST_THREADS=${RUST_TEST_THREADS:-4}" \
        -e CMAKE_BUILD_PARALLEL_LEVEL=1 \
        -v "$SOURCE:/source" \
        -v "$WORK:/work" \
        -v "$SCRIPT_PATH:/script:ro" \
        "$CONTAINER_IMAGE" \
        bash /script \
        --container-body \
        --source /source \
        --work /work/container-work \
        --target "$TARGET" \
        --suffix "$SUFFIX" \
        --version "$VERSION" \
        --expected-commit "$EXPECTED_COMMIT" \
        --validation-commit "$VALIDATION_COMMIT" \
        2>&1 | tee "$WORK/container-run.log"
    rc=${PIPESTATUS[0]}
    set -e

    if [ "$rc" -ne 0 ]; then
        echo "container candidate run failed with rc=$rc" \
            >"$WORK/container-failure.txt"
        exit "$rc"
    fi

    mv "$inner_work/evidence" "$WORK/evidence"
    cp "$WORK/container-orchestration.txt" \
        "$WORK/evidence/container-orchestration.txt"
    cp "$WORK/docker-pull.log" "$WORK/evidence/docker-pull.log"
    cp "$WORK/container-run.log" "$WORK/evidence/container-run.log"
}

record_environment() {
    {
        echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "mode=$MODE"
        echo "hostname=$(uname -n)"
        echo "arch=$(uname -m)"
        echo "uname=$(uname -a)"
        echo "nproc=$(nproc)"
        echo "os_release_begin"
        cat /etc/os-release
        echo "os_release_end"
        echo "debian_version=$(cat /etc/debian_version 2>/dev/null || true)"
        echo "libc=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
        echo "memory_begin"
        if command -v free >/dev/null 2>&1; then
            free -h
        else
            echo "free=unavailable"
        fi
        echo "memory_end"
        echo "rustc_begin"
        rustc -vV
        echo "rustc_end"
        echo "cargo=$(cargo --version)"
        echo "rustup=$(rustup --version 2>&1 | awk 'NR==1')"
        echo "cc=$("$CC" --version | awk 'NR==1')"
        echo "cxx=$("$CXX" --version | awk 'NR==1')"
        echo "cmake=$(cmake --version | awk 'NR==1')"
        echo "cmake_generator=${CMAKE_GENERATOR-}"
        echo "make=$(make --version | awk 'NR==1')"
        echo "protoc=$(protoc --version 2>&1)"
        echo "readelf=$(readelf --version | awk 'NR==1')"
        echo "strip=$(strip --version | awk 'NR==1')"
        echo "tar=$(tar --version | awk 'NR==1')"
        echo "xz=$(xz --version | awk 'NR==1')"
        echo "sha256sum=$(sha256sum --version | awk 'NR==1')"
        echo "env_begin"
        env | grep -E '^(CARGO|RUST|CMAKE|CC|CXX|MBX|PATH)=' |
            sort || true
        echo "env_end"
    } >"$EVIDENCE/env.txt"
}

record_source_identity() {
    local head
    local pin_count
    local dirty

    head=$(git -C "$SOURCE" rev-parse HEAD)
    assert_eq "$head" "$EXPECTED_COMMIT" "source commit"
    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source worktree must be clean"

    pin_count=$(grep -c '40f15ed1b71b66950961b175829fdf17a763f108' \
        "$SOURCE/Cargo.lock" || true)
    assert_eq "$pin_count" "23" "Lance pin reference count"
    grep -q '^rust-version = "1.94.0"$' "$SOURCE/Cargo.toml" ||
        fail "Cargo.toml does not declare Rust 1.94.0"

    {
        echo "source_commit=$head"
        echo "source_tree=$(git -C "$SOURCE" rev-parse HEAD^{tree})"
        echo "source_subject=$(git -C "$SOURCE" log -1 --pretty=%s)"
        echo "source_status_porcelain=$dirty"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "validation_script_sha256=$(sha256sum "$SCRIPT_PATH" | awk '{print $1}')"
        echo "lance_pin_count=$pin_count"
        echo "remote_main=$(
            git -C "$SOURCE" ls-remote \
                https://github.com/lance-format/lance-c refs/heads/main |
                awk '{print $1}' || true
        )"
        echo "input_tree_begin"
        git -C "$SOURCE" ls-tree HEAD -- \
            src include Cargo.lock Cargo.toml build.rs CMakeLists.txt \
            .github/scripts/verify-linux-staging.sh \
            .github/scripts/verify-shared-exports.sh \
            .github/scripts/strip-release-shared.sh \
            .github/scripts/verify-cmake-consumer.sh \
            examples/cmake-consumer test_data/v0.27.1/pq_in_schema
        echo "input_tree_end"
        echo "source_files_sha256=$(
            git -C "$SOURCE" ls-tree -r HEAD -- src include |
                sha256sum | awk '{print $1}'
        )"
    } >"$EVIDENCE/source-identity.txt"
}

check_toolchain() {
    local rustc_version
    local rustc_release
    local cargo_version

    rustc_version=$(rustc -vV)
    rustc_release=$(printf '%s\n' "$rustc_version" |
        sed -n 's/^release: //p')
    assert_eq "$rustc_release" "1.94.0" "rustc release"
    cargo_version=$(cargo --version)
    case "$cargo_version" in
        "cargo 1.94.0 "*) ;;
        *) fail "cargo is not 1.94.0: $cargo_version" ;;
    esac
}

configure_release() {
    cmake -S "$SOURCE" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release
}

build_release() {
    cmake --build "$WORK/build" --parallel 1
}

install_release() {
    cmake --install "$WORK/build" --prefix "$WORK/stage"
}

record_release_artifacts() {
    local release_so
    local release_a

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
    sha256sum \
        "$WORK/stage/lib/liblance_c.a" \
        "$WORK/stage/lib/liblance_c.so" \
        >"$EVIDENCE/stage-prestrip.sha256"
    stat -c '%n %s' \
        "$WORK/stage/lib/liblance_c.a" \
        "$WORK/stage/lib/liblance_c.so" \
        >>"$EVIDENCE/stage-prestrip.sha256"
}

strip_shared() {
    bash "$SOURCE/.github/scripts/strip-release-shared.sh" \
        "$WORK/stage" linux
}

verify_static_untouched() {
    local before
    local after

    before=$(awk '$2 ~ /liblance_c\.a$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    after=$(sha256sum "$WORK/stage/lib/liblance_c.a" | awk '{print $1}')
    assert_eq "$after" "$before" "static archive changed during shared strip"

    readelf -S "$WORK/stage/lib/liblance_c.so" \
        >"$EVIDENCE/readelf-sections.txt"
    if grep -q '\.symtab' "$EVIDENCE/readelf-sections.txt"; then
        fail "shared library still contains a .symtab section"
    fi
    if command -v file >/dev/null 2>&1; then
        file "$WORK/stage/lib/liblance_c.so" >"$EVIDENCE/file-shared.txt"
        file "$WORK/stage/lib/liblance_c.a" >"$EVIDENCE/file-static.txt"
    else
        echo "file=unavailable" >"$EVIDENCE/file-shared.txt"
        echo "file=unavailable" >"$EVIDENCE/file-static.txt"
    fi
}

verify_exports() {
    bash "$SOURCE/.github/scripts/verify-shared-exports.sh" \
        "$WORK/stage/lib/liblance_c.so" \
        "$WORK/stage/include/lance/lance.h" linux
}

record_export_sets() {
    local library="$WORK/stage/lib/liblance_c.so"
    local header="$WORK/stage/include/lance/lance.h"
    local expected
    local actual

    expected=$(sed '/^[[:space:]]*\*/d' "$header" |
        grep -oE 'lance_[A-Za-z0-9_]+[[:space:]]*\(' |
        sed 's/[[:space:]]*($//' |
        sort -u |
        wc -l)
    actual=$(nm -D --defined-only -j "$library" |
        sed 's/@.*//' |
        sort -u |
        wc -l)
    assert_eq "$actual" "$expected" "export count does not match header"

    {
        echo "header_declarations=$expected"
        echo "defined_dynamic_exports=$actual"
    } >"$EVIDENCE/export-count.txt"

    nm -D --defined-only -j "$library" |
        sed 's/@.*//' |
        sort -u |
        grep -v '^lance_' >"$EVIDENCE/non-lance-exports.txt" || true
    if [ -s "$EVIDENCE/non-lance-exports.txt" ]; then
        fail "shared library exports non-lance symbols"
    fi
}

verify_installed_headers() {
    diff -r "$SOURCE/include/lance" "$WORK/stage/include/lance"
    cmp "$SOURCE/include/lance/lance.h" \
        "$WORK/stage/include/lance/lance.h"
}

verify_linux_staging() {
    bash "$SOURCE/.github/scripts/verify-linux-staging.sh" \
        "$WORK/stage" "$TARGET"
}

record_elf_metadata() {
    local library="$WORK/stage/lib/liblance_c.so"

    readelf -h "$library" >"$EVIDENCE/readelf-header.txt"
    readelf -d "$library" >"$EVIDENCE/readelf-dynamic.txt"
    readelf -l "$library" >"$EVIDENCE/readelf-program-headers.txt"
    readelf --version-info "$library" >"$EVIDENCE/readelf-version-info.txt"
    ldd "$library" >"$EVIDENCE/ldd.txt" 2>&1 || true

    case "$TARGET" in
        x86_64-unknown-linux-gnu)
            grep -q 'Machine:.*X86-64' "$EVIDENCE/readelf-header.txt" ||
                fail "ELF machine is not X86-64"
            ;;
        aarch64-unknown-linux-gnu)
            grep -q 'Machine:.*AArch64' "$EVIDENCE/readelf-header.txt" ||
                fail "ELF machine is not AArch64"
            ;;
    esac

    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
        "$EVIDENCE/readelf-version-info.txt" |
        sort -uV >"$EVIDENCE/glibc-versions.txt"
    [ -s "$EVIDENCE/glibc-versions.txt" ] ||
        fail "no GLIBC symbol versions found"
    tail -1 "$EVIDENCE/glibc-versions.txt" >"$EVIDENCE/glibc-max.txt"

    if grep -Eq 'GLIBCXX_|CXXABI_|GCC_[0-9]' \
        "$EVIDENCE/readelf-version-info.txt"; then
        fail "shared library references a C++ or GCC symbol version"
    fi
}

verify_consumer_log() {
    local log=$1
    local static_reads
    local shared_reads
    local passes
    local clone_lines
    local error_lines
    local scan_lines
    local create_lines

    [ -f "$log" ] || fail "missing consumer log: $log"
    static_reads=$(grep -c \
        'Verified static consumer dataset read: rows: 512, version: 2' \
        "$log" || true)
    shared_reads=$(grep -c \
        'Verified shared consumer dataset read: rows: 512, version: 2' \
        "$log" || true)
    passes=$(grep -c 'acceptance: PASS' "$log" || true)
    clone_lines=$(grep -c \
        'acceptance: clone rows=4 source_closed_before_use=yes' \
        "$log" || true)
    error_lines=$(grep -c \
        'acceptance: last_error code=3 message_bytes=' "$log" || true)
    scan_lines=$(grep -c \
        'acceptance: scan rows=4 batches=2 array_releases=2 schema_releases=1 stream_releases=1' \
        "$log" || true)
    create_lines=$(grep -c \
        'acceptance: create rows=4 version=1' "$log" || true)

    assert_eq "$static_reads" "1" "static historical dataset reads"
    assert_eq "$shared_reads" "1" "shared historical dataset reads"
    assert_eq "$passes" "2" "acceptance PASS lines"
    assert_eq "$clone_lines" "2" "clone-lifetime acceptance lines"
    assert_eq "$error_lines" "2" "last-error acceptance lines"
    assert_eq "$create_lines" "2" "create acceptance lines"
    if [ "$scan_lines" -lt 4 ]; then
        fail "expected at least four scan acceptance lines, found $scan_lines"
    fi
}

run_installed_consumer() {
    bash "$SOURCE/.github/scripts/verify-cmake-consumer.sh" \
        --from-install-prefix "$WORK/stage"
}

create_archive() {
    local archive=$1
    (
        cd "$EVIDENCE" || exit 1
        XZ_OPT=-T0 tar -C "$WORK/stage" -cJf "$(basename "$archive")" .
        shasum -a 256 "$(basename "$archive")" \
            >"$(basename "$archive").sha256"
        shasum -a 512 "$(basename "$archive")" \
            >"$(basename "$archive").sha512"
    )
}

verify_archive_checksums() {
    local archive=$1
    (
        cd "$EVIDENCE" || exit 1
        shasum -a 256 -c "$(basename "$archive").sha256"
        shasum -a 512 -c "$(basename "$archive").sha512"
    )
}

extract_archive() {
    local archive=$1
    rm -rf "$WORK/roundtrip"
    mkdir -p "$WORK/roundtrip"
    tar -C "$WORK/roundtrip" -xJf "$archive"
}

check_archive_members() {
    local archive=$1
    tar -tf "$archive" >"$EVIDENCE/archive-members.txt"
    tar -tvJf "$archive" >"$EVIDENCE/archive-members-verbose.txt"
    if grep -Eq '^/|(^|/)\.\.(/|$)' "$EVIDENCE/archive-members.txt"; then
        fail "archive contains an absolute or parent-relative path"
    fi
}

write_manifest() {
    local root=$1
    local output=$2
    if find "$root" -type l | grep -q .; then
        fail "unexpected symlink under $root"
    fi
    (
        cd "$root" || exit 1
        find . -type f -print0 | sort -z | xargs -0 sha256sum
    ) >"$output"
}

write_paths() {
    local root=$1
    local output=$2
    (
        cd "$root" || exit 1
        find . -mindepth 1 | sort
    ) >"$output"
}

compare_archive_content() {
    diff -u "$EVIDENCE/stage.manifest" "$EVIDENCE/roundtrip.manifest"
    diff -u "$EVIDENCE/stage.paths" "$EVIDENCE/roundtrip.paths"
}

verify_archive_header() {
    cmp "$SOURCE/include/lance/lance.h" \
        "$WORK/roundtrip/include/lance/lance.h"
}

run_archive_consumer() {
    bash "$SOURCE/.github/scripts/verify-cmake-consumer.sh" \
        --from-install-prefix "$WORK/roundtrip"
}

verify_source_unchanged() {
    local dirty

    dirty=$(git -C "$SOURCE" status --porcelain | wc -l)
    assert_eq "$dirty" "0" "source worktree changed during validation"
}

cleanup_success() {
    rm -rf "$WORK/build" "$WORK/stage" "$WORK/roundtrip"
}

write_summary() {
    local archive=$1
    local archive_name
    local static_sha
    local static_bytes
    local shared_before
    local shared_before_bytes
    local shared_after
    local shared_after_bytes
    local export_line
    local dt_needed

    archive_name=$(basename "$archive")
    static_sha=$(sha256sum "$WORK/stage/lib/liblance_c.a" | awk '{print $1}')
    static_bytes=$(awk '$1 ~ /liblance_c\.a$/ {print $2}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_before=$(awk '$2 ~ /liblance_c\.so$/ {print $1}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_before_bytes=$(awk '$1 ~ /liblance_c\.so$/ {print $2}' \
        "$EVIDENCE/stage-prestrip.sha256")
    shared_after=$(sha256sum "$WORK/stage/lib/liblance_c.so" | awk '{print $1}')
    shared_after_bytes=$(stat -c %s "$WORK/stage/lib/liblance_c.so")
    export_line=$(grep '^Verified ' "$LOG_DIR/exports.log" | tail -1)
    dt_needed=$(sed -n \
        's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' \
        "$EVIDENCE/readelf-dynamic.txt" | paste -sd, -)

    {
        echo "status=pass"
        echo "target=$TARGET"
        echo "suffix=$SUFFIX"
        echo "version=$VERSION"
        echo "source_commit=$EXPECTED_COMMIT"
        echo "validation_commit=$VALIDATION_COMMIT"
        echo "hostname=$(uname -n)"
        echo "host_arch=$(uname -m)"
        echo "host_os=$(grep '^PRETTY_NAME=' /etc/os-release |
            cut -d= -f2- | tr -d '"')"
        echo "libc=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
        echo "rustc=$(rustc -V)"
        echo "cargo=$(cargo --version)"
        echo "cmake=$(cmake --version | awk 'NR==1')"
        echo "cc=$("$CC" --version | awk 'NR==1')"
        echo "protoc=$(protoc --version 2>&1)"
        echo "archive=$archive_name"
        echo "archive_sha256=$(awk '{print $1}' "$archive.sha256")"
        echo "archive_sha512=$(awk '{print $1}' "$archive.sha512")"
        echo "archive_bytes=$(stat -c %s "$archive")"
        echo "stage_manifest_sha256=$(
            sha256sum "$EVIDENCE/stage.manifest" | awk '{print $1}'
        )"
        echo "archive_member_count=$(wc -l <"$EVIDENCE/archive-members.txt")"
        echo "static_sha256=$static_sha"
        echo "static_bytes=$static_bytes"
        echo "shared_unstripped_sha256=$shared_before"
        echo "shared_unstripped_bytes=$shared_before_bytes"
        echo "shared_stripped_bytes=$shared_after_bytes"
        echo "shared_stripped_sha256=$shared_after"
        echo "shared_stripped_file=$(cat "$EVIDENCE/file-shared.txt")"
        echo "exports=$export_line"
        echo "header_declarations=$(sed -n 's/^header_declarations=//p' \
            "$EVIDENCE/export-count.txt")"
        echo "defined_dynamic_exports=$(sed -n \
            's/^defined_dynamic_exports=//p' "$EVIDENCE/export-count.txt")"
        echo "max_glibc=$(cat "$EVIDENCE/glibc-max.txt")"
        echo "dt_needed=$dt_needed"
        echo "consumer_static_dataset=rows:512,version:2"
        echo "consumer_shared_dataset=rows:512,version:2"
        echo "consumer_acceptance=PASS"
        echo "archive_roundtrip_content=identical"
        echo "archive_roundtrip_consumer=PASS"
        echo "archive_header_matches_source=yes"
    } >"$EVIDENCE/summary.env"
}

run_evidence() {
    local archive_name
    local archive

    if [ "$MODE" = container-body ]; then
        setup_debian_container
    fi

    export CC=${CC:-clang}
    export CXX=${CXX:-clang++}
    export CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-8}
    export RUST_TEST_THREADS=${RUST_TEST_THREADS:-4}
    export CMAKE_BUILD_PARALLEL_LEVEL=1
    export RUSTUP_TOOLCHAIN=1.94.0

    mkdir -p "$EVIDENCE" "$LOG_DIR"
    : >"$STATUS_FILE"
    trap on_exit EXIT

    record_environment
    record_source_identity
    check_toolchain

    run_step configure-release configure_release
    run_step build-release build_release
    run_step install-release install_release
    run_step record-release-artifacts record_release_artifacts
    run_step record-stage-prestrip record_stage_prestrip

    run_step strip-shared strip_shared
    run_step static-untouched verify_static_untouched
    sha256sum "$WORK/stage/lib/liblance_c.so" \
        >"$WORK/shared-poststrip.sha256"
    mv "$WORK/shared-poststrip.sha256" \
        "$EVIDENCE/stage-poststrip.sha256"

    run_step exports verify_exports
    run_step record-export-sets record_export_sets
    run_step installed-headers verify_installed_headers
    run_step linux-staging verify_linux_staging
    run_step elf-metadata record_elf_metadata

    run_step consumer-installed run_installed_consumer
    verify_consumer_log "$LOG_DIR/consumer-installed.log"

    archive_name="lance-c-v${VERSION}-${TARGET}${SUFFIX}.tar.xz"
    archive="$EVIDENCE/$archive_name"
    run_step archive create_archive "$archive"
    run_step archive-checksums verify_archive_checksums "$archive"
    run_step archive-members check_archive_members "$archive"
    run_step archive-extract extract_archive "$archive"

    run_step stage-manifest write_manifest \
        "$WORK/stage" "$EVIDENCE/stage.manifest"
    run_step roundtrip-manifest write_manifest \
        "$WORK/roundtrip" "$EVIDENCE/roundtrip.manifest"
    run_step stage-paths write_paths "$WORK/stage" "$EVIDENCE/stage.paths"
    run_step roundtrip-paths write_paths \
        "$WORK/roundtrip" "$EVIDENCE/roundtrip.paths"
    run_step archive-content compare_archive_content
    run_step archive-header verify_archive_header

    run_step consumer-archive run_archive_consumer
    verify_consumer_log "$LOG_DIR/consumer-archive.log"
    run_step source-unchanged verify_source_unchanged

    run_step write-summary write_summary "$archive"
    run_step cleanup-success cleanup_success
}

if [ "$MODE" = outer ] && [ -n "$CONTAINER_IMAGE" ]; then
    run_container_outer
else
    run_evidence
fi
