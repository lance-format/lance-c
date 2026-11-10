#!/usr/bin/env bash
set -euo pipefail

repo=${1:?repository checkout}
shared=${2:?True or False}
log_dir=${3:?log directory}

conan_bin=/home/xuanwo/work/lance-c-100/conan-venv/bin/conan
export CONAN_HOME=/home/xuanwo/work/lance-c-100/conan-home
export CARGO_BUILD_JOBS=8
export RUST_TEST_THREADS=4
export PATH="/home/xuanwo/.cargo/bin:/home/xuanwo/.local/bin:${PATH}"

mkdir -p "${log_dir}"
log="${log_dir}/conan-source-shared-${shared}.log"

set +e
"${conan_bin}" create "${repo}/recipes/lance-c/all" \
    --version=0.1.9 \
    -o "lance-c/*:from_source=True" \
    -o "lance-c/*:shared=${shared}" \
    >"${log}" 2>&1
status=$?
set -e

printf '%s\n' "${status}" >"${log}.exit"
exit "${status}"
