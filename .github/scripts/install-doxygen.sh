#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Install the pinned Doxygen release used to validate the public API reference.
#
# The version is pinned because Doxyfile turns documentation warnings into
# errors: local runs and CI must use the same parser, or a new warning from a
# newer Doxygen would fail an unrelated change. Bump DOXYGEN_VERSION and
# DOXYGEN_SHA256 together, then recheck the warning set with
# .github/scripts/check-doxygen.sh.

set -euo pipefail

DOXYGEN_VERSION="1.12.0"
DOXYGEN_SHA256="3c42c3f3fb206732b503862d9c9c11978920a8214f223a3950bbf2520be5f647"

# The release archive is a dynamically linked Linux x86_64 binary. Other hosts
# must install Doxygen themselves and pass DOXYGEN_BIN to check-doxygen.sh.
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ;;
    *)
        echo "error: the pinned Doxygen binary supports Linux x86_64 only; install Doxygen ${DOXYGEN_VERSION} manually and set DOXYGEN_BIN" >&2
        exit 1
        ;;
esac

archive="doxygen-${DOXYGEN_VERSION}.linux.bin.tar.gz"
download_url="https://github.com/doxygen/doxygen/releases/download/Release_${DOXYGEN_VERSION//./_}/${archive}"
install_root="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/lance-c-doxygen"
bin_dir="${install_root}/doxygen-${DOXYGEN_VERSION}/bin"

verify_sha256() {
    local file="$1"
    local expected="$2"
    local actual

    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$file" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        actual="$(shasum -a 256 "$file" | awk '{print $1}')"
    else
        echo "error: neither sha256sum nor shasum is available to verify ${archive}" >&2
        exit 1
    fi

    if [[ "$actual" != "$expected" ]]; then
        echo "error: checksum mismatch for ${archive}" >&2
        echo "  expected: ${expected}" >&2
        echo "  actual:   ${actual}" >&2
        exit 1
    fi
}

if [[ ! -x "${bin_dir}/doxygen" ]]; then
    mkdir -p "$install_root"
    download="${install_root}/${archive}"

    echo "Downloading Doxygen ${DOXYGEN_VERSION} from ${download_url}"
    curl -fsSL --retry 3 --retry-all-errors --retry-delay 2 \
        "$download_url" -o "$download"
    verify_sha256 "$download" "$DOXYGEN_SHA256"
    tar -xzf "$download" -C "$install_root" \
        "doxygen-${DOXYGEN_VERSION}/bin/doxygen"
    rm -f "$download"
fi

"${bin_dir}/doxygen" --version
echo "Installed Doxygen ${DOXYGEN_VERSION} in ${bin_dir}"

if [[ -n "${GITHUB_PATH:-}" ]]; then
    echo "$bin_dir" >> "$GITHUB_PATH"
else
    echo "Add ${bin_dir} to PATH, or set DOXYGEN_BIN=${bin_dir}/doxygen"
fi
