#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Download and statically inspect the v0.1.9 release archives. This script
# performs no builds and runs no target-platform binaries.

set -euo pipefail

version="0.1.9"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
root="${1:?usage: audit-artifacts.sh <work-root>}"
release_url="https://github.com/lance-format/lance-c/releases/download/v${version}"

assets=(
    "lance-c-v${version}-x86_64-unknown-linux-gnu.tar.xz"
    "lance-c-v${version}-x86_64-unknown-linux-gnu-debian12.tar.xz"
    "lance-c-v${version}-aarch64-unknown-linux-gnu.tar.xz"
    "lance-c-v${version}-aarch64-unknown-linux-gnu-debian12.tar.xz"
    "lance-c-v${version}-aarch64-apple-darwin.tar.xz"
)

mkdir -p "${root}/assets" "${root}/extracted" "${root}/logs"

{
    echo "host=$(hostname)"
    echo "kernel=$(uname -srm)"
    echo "source_commit=$(git -C "${repo_root}" rev-parse HEAD)"
    echo "source_branch=$(git -C "${repo_root}" rev-parse --abbrev-ref HEAD)"
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"${root}/logs/run-context.txt"

cd "${root}/assets"
for asset in "${assets[@]}"; do
    for suffix in "" ".sha256" ".sha512"; do
        curl --fail --location --retry 3 --silent --show-error \
            --output "${asset}${suffix}" "${release_url}/${asset}${suffix}"
    done
done
curl --fail --location --retry 3 --silent --show-error \
    --output SHA512SUMS "${release_url}/SHA512SUMS"

{
    echo "== sha256 =="
    for asset in "${assets[@]}"; do
        sha256sum --check "${asset}.sha256"
    done
    echo "== sha512 =="
    for asset in "${assets[@]}"; do
        sha512sum --check "${asset}.sha512"
    done
    echo "== aggregate SHA512SUMS =="
    sha512sum --check SHA512SUMS
} 2>&1 | tee "${root}/logs/checksums.txt"

{
    for asset in "${assets[@]}" SHA512SUMS; do
        sha256sum "${asset}"
        sha512sum "${asset}"
        stat --format='%n %s bytes' "${asset}"
    done
} >"${root}/logs/downloaded-digests.txt"

for asset in "${assets[@]}"; do
    name="${asset%.tar.xz}"
    mkdir -p "${root}/extracted/${name}"
    tar --list --xz --file "${asset}" >"${root}/logs/${name}.contents.txt"
    tar --extract --xz --file "${asset}" --directory "${root}/extracted/${name}"
    find "${root}/extracted/${name}" -type f -printf '%P\t%s\n' |
        sort >"${root}/logs/${name}.files.txt"
done

elf_libraries=(
    "lance-c-v${version}-aarch64-unknown-linux-gnu"
    "lance-c-v${version}-aarch64-unknown-linux-gnu-debian12"
)
for name in "${elf_libraries[@]}"; do
    library="${root}/extracted/${name}/lib/liblance_c.so"
    test -f "${library}"
    {
        echo "== file =="
        file "${library}" 2>/dev/null || true
        echo "== ELF header =="
        readelf --file-header "${library}"
        echo "== dynamic section =="
        readelf --dynamic "${library}"
        echo "== version info =="
        readelf --version-info "${library}"
        echo "== max required GLIBC =="
        readelf --version-info "${library}" |
            grep -o 'GLIBC_[0-9][0-9.]*' | sort --version-sort | tail -n 1
        echo "== max required GLIBCXX =="
        readelf --version-info "${library}" |
            grep -o 'GLIBCXX_[0-9][0-9.]*' | sort --version-sort | tail -n 1 || true
        echo "== max required CXXABI =="
        readelf --version-info "${library}" |
            grep -o 'CXXABI_[0-9][0-9.]*' | sort --version-sort | tail -n 1 || true
    } >"${root}/logs/${name}.elf-metadata.txt" 2>&1
done

name="lance-c-v${version}-aarch64-apple-darwin"
library="${root}/extracted/${name}/lib/liblance_c.dylib"
test -f "${library}"
{
    echo "== llvm-lipo info =="
    llvm-lipo-18 -info "${library}"
    echo "== dylib id =="
    llvm-objdump-18 --macho --dylib-id "${library}"
    echo "== rpaths =="
    llvm-objdump-18 --macho --rpaths "${library}"
    echo "== dylibs used =="
    llvm-objdump-18 --macho --dylibs-used "${library}"
    echo "== private headers =="
    llvm-objdump-18 --macho --private-headers "${library}"
} >"${root}/logs/${name}.macho-metadata.txt" 2>&1

{
    echo "completed_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "status=ok"
} >"${root}/logs/status.txt"
cat "${root}/logs/status.txt"
