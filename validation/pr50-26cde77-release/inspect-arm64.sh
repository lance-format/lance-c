#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Host-side inspection of the digest-pinned aarch64 release image.
#
# The aarch64 release leg cannot be executed here (no aarch64 emulation and no
# arm64 host), so this script only verifies facts that can be established
# without executing the image: pinned digest, platform metadata, installed
# tooling needed by .github/workflows/release.yml, the clang installer's arm64
# mapping, and the image's glibc floor.
#
# Usage: EVID_ROOT=<evidence dir> ./inspect-arm64.sh

set -euo pipefail

EVID_ROOT=${EVID_ROOT:-$HOME/Code/evidence/lance-c-pr50-final-26cde77}
SOURCE=${SOURCE:-}
IMAGE=${IMAGE:-quay.io/pypa/manylinux_2_28_aarch64@sha256:34a5e614a4eb26f73f8bf44cbbf2acf8011620453d03ea35a58537bb3cde44de}
if [ -z "${CLANG_VERSION:-}" ] && [ -n "$SOURCE" ]; then
    CLANG_VERSION=$(grep -oE 'manylinux-install-clang -v [^ ]+' \
        "$SOURCE/.github/workflows/release.yml" | head -1 | awk '{print $NF}')
fi
CLANG_VERSION=${CLANG_VERSION:?set CLANG_VERSION or SOURCE to resolve the pinned clang version}
OUT=$EVID_ROOT/arm64
mkdir -p "$OUT"

docker manifest inspect "$IMAGE" >"$OUT/manifest.json"

docker pull --platform linux/arm64 "$IMAGE" 2>&1 | tail -2

{
    printf 'image_ref=%s\n' "$IMAGE"
    docker image inspect --format 'image_id={{.Id}} arch={{.Architecture}} os={{.Os}}' "$IMAGE"
    python3 - "$OUT/manifest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print('manifest_mediaType=%s schemaVersion=%s' % (d.get('mediaType'), d.get('schemaVersion')))
if 'manifests' in d:
    for m in d['manifests']:
        p = m.get('platform', {})
        print('manifest_platform=%s/%s variant=%s digest=%s' % (
            p.get('os'), p.get('architecture'), p.get('variant', ''), m.get('digest', '')))
else:
    print('manifest_platform=single-manifest config=%s' % (d.get('config', {}).get('digest', '')))
PY
} | tee "$OUT/image-meta.env"

cid=$(docker create --platform linux/arm64 "$IMAGE" /bin/true)
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT

docker export "$cid" | tar -t >"$OUT/rootfs-listing.txt"
printf 'rootfs_entries=%s\n' "$(wc -l <"$OUT/rootfs-listing.txt")" | tee -a "$OUT/image-meta.env"

{
    for path in usr/local/bin/cmake usr/local/bin/git usr/bin/unzip usr/bin/curl usr/bin/xz \
        usr/bin/tar usr/bin/sha256sum usr/bin/sha512sum usr/local/bin/manylinux-install-clang \
        usr/bin/make opt/_internal/build_scripts/static_clang_versions.txt \
        opt/_internal/build_scripts/install-static-clang-helper.sh; do
        if grep -qxF "$path" "$OUT/rootfs-listing.txt" || grep -qxF "$path/" "$OUT/rootfs-listing.txt"; then
            printf '%s=present\n' "$path"
        else
            printf '%s=MISSING\n' "$path"
        fi
    done
} >"$OUT/tool-presence.env"

docker cp -L "$cid:/etc/os-release" "$OUT/os-release.txt"
docker cp -L "$cid:/lib64/libc.so.6" "$OUT/libc.so.6"
docker cp -L "$cid:/usr/local/bin/manylinux-install-clang" "$OUT/install-static-clang-helper.sh"
docker cp "$cid:/opt/_internal/build_scripts/static_clang_versions.txt" "$OUT/static_clang_versions.txt"

{
    printf 'os_release=%s\n' "$(grep -E '^(PRETTY_NAME|VERSION_ID)=' "$OUT/os-release.txt" | cut -d= -f2- | tr -d '\"' | tr '\n' ' ')"
    printf 'glibc_version_string=%s\n' "$(strings "$OUT/libc.so.6" | grep -m1 -E 'GNU C Library.*release version [0-9]' || echo unknown)"
    printf 'libc_soname=%s\n' "$(readelf -d "$OUT/libc.so.6" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p')"
    printf 'clang_installer_supports_arm64=%s\n' \
        "$(grep -cE 'ARM64\|aarch64\|arm64\)' "$OUT/install-static-clang-helper.sh")"
    printf 'clang_installer_arm64_mapping=%s\n' \
        "$(grep -m1 'GO_ARCH=arm64' "$OUT/install-static-clang-helper.sh" || echo missing)"
    printf 'toolchain_path=%s\n' "$(grep -m1 '^TOOLCHAIN_PATH=' "$OUT/install-static-clang-helper.sh")"
    # The installer prepends the "v" itself, so accept the pin with or without it.
    if grep -qE "^v?${CLANG_VERSION} " "$OUT/static_clang_versions.txt"; then
        printf 'clang_version_%s=listed\n' "$CLANG_VERSION"
    else
        printf 'clang_version_%s=MISSING\n' "$CLANG_VERSION"
    fi
    if command -v docker >/dev/null && docker run --rm --platform linux/arm64 --entrypoint /bin/true "$IMAGE" >/dev/null 2>&1; then
        printf 'arm64_execution=available\n'
    else
        printf 'arm64_execution=unavailable\n'
    fi
    printf 'binfmt_entries=%s\n' "$(ls /proc/sys/fs/binfmt_misc 2>/dev/null | tr '\n' ',' || echo none)"
} | tee "$OUT/arm64-facts.env"

cat "$OUT/arm64-facts.env"
