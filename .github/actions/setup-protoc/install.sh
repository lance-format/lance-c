#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

set -euo pipefail

ARCH=$(uname -m)
case "$ARCH" in
  x86_64)
    PA="linux-x86_64"
    SHA256="f853e691868d0557425ea290bf7ba6384eef2fa9b04c323afab49a770ba9da80"
    ;;
  aarch64)
    PA="linux-aarch_64"
    SHA256="9eae1f20f70cccc912d1c318c3929b86aebf5afd4b0f32c196ef682c222ed5ae"
    ;;
  *)
    echo "::error::Unsupported protoc architecture: $ARCH (expected x86_64 or aarch64)" >&2
    exit 1
    ;;
esac
PREFIX=${PROTOC_INSTALL_PREFIX:-/usr/local}
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT
curl -fsSL "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-${PA}.zip" -o "$WORK_DIR/protoc.zip"
echo "$SHA256  $WORK_DIR/protoc.zip" | sha256sum --check --strict
mkdir -p "$PREFIX"
unzip -o "$WORK_DIR/protoc.zip" -d "$PREFIX" bin/protoc 'include/*'
"$PREFIX/bin/protoc" --version
# Keep the install discoverable in later steps without relying on a writable
# system prefix.
if [ -n "${GITHUB_PATH:-}" ]; then
    echo "$PREFIX/bin" >>"$GITHUB_PATH"
fi
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "PROTOC_INCLUDE=$PREFIX/include" >>"$GITHUB_ENV"
fi
