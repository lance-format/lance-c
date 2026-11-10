#!/usr/bin/env bash
set -euo pipefail

repo=${1:?repository checkout}
triplet=${2:?vcpkg triplet}
log_dir=${3:?log directory}

export PATH="/home/xuanwo/work/lance-c-100/tools/root/usr/bin:/home/xuanwo/.cargo/bin:/home/xuanwo/.local/bin:${PATH}"
export VCPKG_ROOT=/home/xuanwo/work/lance-c-100/vcpkg
export VCPKG_DEFAULT_BINARY_CACHE=/home/xuanwo/work/lance-c-100/vcpkg-cache
export VCPKG_MAX_CONCURRENCY=8
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4

mkdir -p "${log_dir}"
log="${log_dir}/vcpkg-source-${triplet}.log"

set +e
"${VCPKG_ROOT}/vcpkg" install "lance-c[from-source]" \
    --overlay-ports="${repo}/ports" \
    --triplet "${triplet}" \
    >"${log}" 2>&1
status=$?
set -e

printf '%s\n' "${status}" >"${log}.exit"
exit "${status}"
