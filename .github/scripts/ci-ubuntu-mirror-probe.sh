#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail

image="ubuntu:24.04"
root="${1:?usage: ci-ubuntu-mirror-probe.sh EVIDENCE_DIR}"

mkdir -p "$root"
exec > >(tee "$root/probe.log") 2>&1

printf '%s\n' \
  $'https://mirrors.edge.kernel.org/ubuntu/\tpriority:1' \
  $'http://azure.archive.ubuntu.com/ubuntu/\tpriority:2' \
  $'https://archive.ubuntu.com/ubuntu/\tpriority:3' \
  $'https://security.ubuntu.com/ubuntu/\tpriority:4' \
  > "$root/apt-mirrors.txt"

printf '%s\n' \
  $'http://127.0.0.1:9/ubuntu/\tpriority:1' \
  $'https://mirrors.edge.kernel.org/ubuntu/\tpriority:2' \
  $'http://azure.archive.ubuntu.com/ubuntu/\tpriority:3' \
  $'https://archive.ubuntu.com/ubuntu/\tpriority:4' \
  $'https://security.ubuntu.com/ubuntu/\tpriority:5' \
  > "$root/apt-mirrors-invalid-first.txt"

printf '%s\n' \
  $'http://127.0.0.1:9/ubuntu/\tpriority:1' \
  > "$root/apt-mirrors-all-invalid.txt"

printf '%s\n' \
  'Types: deb' \
  'URIs: mirror+file:/etc/apt/apt-mirrors.txt' \
  'Suites: noble noble-updates noble-backports' \
  'Components: main universe restricted multiverse' \
  'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
  '' \
  'Types: deb' \
  'URIs: mirror+file:/etc/apt/apt-mirrors.txt' \
  'Suites: noble-security' \
  'Components: main universe restricted multiverse' \
  'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
  > "$root/ubuntu.sources"

date -u
hostname
uname -a
docker version --format '{{.Server.Version}}'
docker pull "$image"
docker image inspect "$image" --format '{{.Id}} {{.RepoDigests}}' | tee "$root/image.txt"
docker run --rm "$image" cat /etc/apt/sources.list.d/ubuntu.sources > "$root/base-ubuntu.sources"

run_case() {
  local name="$1"
  local mirror_file="$2"
  local expected="$3"
  local case_dir="$root/$name"
  local status

  mkdir -p "$case_dir"
  set +e
  docker run --rm \
    --volume "$root/ubuntu.sources:/etc/apt/sources.list.d/ubuntu.sources:ro" \
    --volume "$mirror_file:/etc/apt/apt-mirrors.txt:ro" \
    --volume /etc/ssl/certs:/etc/ssl/certs:ro \
    "$image" \
    bash -euxo pipefail -c '
      cat /etc/apt/sources.list.d/ubuntu.sources
      cat /etc/apt/apt-mirrors.txt
      apt-get update
      DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends protobuf-compiler lld
      dpkg-query -W protobuf-compiler lld
      protoc --version
      ld.lld --version
      apt-cache policy protobuf-compiler lld
    ' 2>&1 | tee "$case_dir/run.log"
  status="${PIPESTATUS[0]}"
  set -e

  printf '%s\n' "$status" > "$case_dir/status.txt"
  if [ "$expected" = "success" ]; then
    test "$status" -eq 0
  else
    test "$status" -ne 0
  fi
}

run_case normal "$root/apt-mirrors.txt" success
run_case fallback "$root/apt-mirrors-invalid-first.txt" success
run_case all-invalid "$root/apt-mirrors-all-invalid.txt" failure

printf '%s\n' "evidence: $root" | tee "$root/summary.txt"
