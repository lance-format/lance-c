#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <new-work-directory>" >&2
  exit 2
fi

example_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${example_dir}/../.." && pwd)"
work_dir="$1"

if [[ -e "${work_dir}" ]]; then
  echo "work directory already exists: ${work_dir}" >&2
  exit 2
fi
mkdir -p "${work_dir}"

if [[ -z "${LANCE_C_LIB:-}" ]]; then
  RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-1.94.0}" \
    cargo build --locked --release --lib --manifest-path "${repo_root}/Cargo.toml"
  LANCE_C_LIB="${repo_root}/target/release/liblance_c.so"
fi
export LANCE_C_LIB

if [[ ! -f "${LANCE_C_LIB}" ]]; then
  echo "lance-c shared library not found: ${LANCE_C_LIB}" >&2
  exit 2
fi

cc -std=c11 -O2 -Wall -Wextra -Werror \
  -I "${repo_root}/include" \
  "${example_dir}/producer.c" \
  -L "$(dirname "${LANCE_C_LIB}")" \
  -Wl,-rpath,"$(dirname "${LANCE_C_LIB}")" \
  -llance_c \
  -o "${work_dir}/producer"

"${work_dir}/producer" create "${work_dir}/dist"
cp -a "${work_dir}/dist" "${work_dir}/baseline"
"${work_dir}/producer" build "${work_dir}/dist" "${work_dir}/metadata" \
  | tee "${work_dir}/producer.log"

sha256sum "${work_dir}"/metadata/*.pb | tee "${work_dir}/metadata.sha256"

{
  java -version
  mvn -version
  RUSTUP_TOOLCHAIN="${RUSTUP_TOOLCHAIN:-1.94.0}" rustc --version
  cc --version | head -1
} > "${work_dir}/toolchain.log" 2>&1

mvn -B -ntp -f "${example_dir}/java/pom.xml" test
mvn -B -ntp -f "${example_dir}/java/pom.xml" dependency:tree \
  -DoutputFile="${work_dir}/dependency-tree.txt"
mvn -B -ntp -f "${example_dir}/java/pom.xml" dependency:build-classpath \
  -Dmdep.outputFile="${work_dir}/java-classpath.txt"

java \
  --add-opens=java.base/java.nio=ALL-UNNAMED \
  -cp "${example_dir}/java/target/classes:$(<"${work_dir}/java-classpath.txt")" \
  org.lance.crosssdk.CrossSdkAcceptance \
  "${work_dir}/dist" \
  "${work_dir}/baseline" \
  "${work_dir}/metadata" \
  | tee "${work_dir}/java-acceptance.log"
