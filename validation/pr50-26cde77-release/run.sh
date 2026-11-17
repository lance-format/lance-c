#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Host-side driver: release-path acceptance for lance-c PR #50 head
# 26cde77e8c1acc538ec1a2f3be26f8eeea74373a.
#
# Runs on NieR. Pulls the digest-pinned manylinux_2_28_x86_64 image, starts a
# pristine container (no extra packages installed), hands the product tree in
# read-only and then runs body.sh inside it as uid 1000.
#
# Usage:
#   SOURCE=<clone at validated commit> EVID_ROOT=<evidence dir> ./run.sh

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOURCE=${SOURCE:?set SOURCE to a lance-c clone checked out at the validated commit}
EVID_ROOT=${EVID_ROOT:-$HOME/Code/evidence/lance-c-pr50-final-26cde77}
IMAGE=${IMAGE:-quay.io/pypa/manylinux_2_28_x86_64@sha256:d086f64cddcace250ef0f0073583c51b8298159303290a730c9f59bd91edfd2c}
EXPECTED_HEAD=${EXPECTED_HEAD:-26cde77e8c1acc538ec1a2f3be26f8eeea74373a}
NAME=${NAME:-lance-c-pr50-26cde77}
RUN_ID=${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}

mkdir -p "$EVID_ROOT/logs" "$EVID_ROOT/driver" "$EVID_ROOT/work"
cp "$HERE/body.sh" "$EVID_ROOT/driver/body.sh"
chmod +x "$EVID_ROOT/driver/body.sh"

source_head=$(git -C "$SOURCE" rev-parse HEAD)
if [ "$source_head" != "$EXPECTED_HEAD" ]; then
    printf 'source checkout is at %s, expected %s\n' "$source_head" "$EXPECTED_HEAD" >&2
    exit 1
fi
# Use the clang version pinned by the workflow under test instead of a copy.
CLANG_VERSION=${CLANG_VERSION:-$(grep -oE 'manylinux-install-clang -v [^ ]+' \
    "$SOURCE/.github/workflows/release.yml" | head -1 | awk '{print $NF}')}
if [ -z "$CLANG_VERSION" ]; then
    printf 'could not resolve the pinned clang version from release.yml\n' >&2
    exit 1
fi
printf '%s\n' "$source_head" >"$EVID_ROOT/driver/source-commit.txt"
git -C "$HERE" rev-parse HEAD >"$EVID_ROOT/driver/validation-commit.txt" 2>/dev/null || true
{
    printf 'run_id=%s\n' "$RUN_ID"
    printf 'image_ref=%s\n' "$IMAGE"
    printf 'clang_version=%s\n' "$CLANG_VERSION"
    printf 'source=%s\n' "$SOURCE"
    printf 'host=%s\n' "$(uname -srm)"
} >"$EVID_ROOT/driver/run-context.env"

docker pull "$IMAGE" 2>&1 | tee "$EVID_ROOT/logs/docker-pull.log" | tail -3
image_id=$(docker image inspect --format '{{.Id}}' "$IMAGE")
printf 'image_id=%s arch=%s\n' "$image_id" \
    "$(docker image inspect --format '{{.Architecture}}' "$IMAGE")" |
    tee "$EVID_ROOT/driver/image.txt"

docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --platform linux/amd64 \
    -v "$SOURCE":/source:ro \
    -v "$EVID_ROOT/work":/work \
    -v "$EVID_ROOT":/evidence \
    -v "$HOME/.local/bin/mbx":/home/xuanwo/.local/bin/mbx:ro \
    -v "$HOME/.local/share/mbx":/home/xuanwo/.local/share/mbx:ro \
    -v "$HOME/.cargo":/home/xuanwo/.cargo \
    -v "$HOME/.rustup":/home/xuanwo/.rustup:ro \
    -v "$HOME/.cache/mbx":/home/xuanwo/.cache/mbx \
    -v "$HOME/.config/mbx":/home/xuanwo/.config/mbx:ro \
    "$IMAGE" bash -lc 'trap "exit 0" TERM INT; sleep infinity & wait' >/dev/null

# Prepare writable install locations and durable evidence ownership. The
# workflow runs as root inside the container; uid 1000 is used here only so the
# host-mounted cargo/cache trees keep their ownership.
docker exec -u 0 "$NAME" bash -lc '
    set -e
    mkdir -p /home/xuanwo /work /evidence
    mkdir -p /usr/local/bin /usr/local/include
    chown 1000:1000 /home/xuanwo /opt /usr/local/bin /usr/local/include
    chown -R 1000:1000 /work /evidence
'

set +e
docker exec \
    --user 1000:1000 \
    -e HOME=/home/xuanwo \
    -e XDG_CACHE_HOME=/home/xuanwo/.cache \
    -e XDG_CONFIG_HOME=/home/xuanwo/.config \
    -e CARGO_HOME=/home/xuanwo/.cargo \
    -e RUSTUP_HOME=/home/xuanwo/.rustup \
    -e RUSTUP_TOOLCHAIN=stable \
    -e CARGO_BUILD_JOBS=8 \
    -e CARGO_TERM_COLOR=never \
    -e CC=clang \
    -e CXX=clang++ \
    -e MACOSX_DEPLOYMENT_TARGET=11.0 \
    -e CLANG_VERSION="$CLANG_VERSION" \
    -e RESUME_FROM="${RESUME_FROM:-}" \
    -e CONTAINER_IMAGE_REF="$IMAGE" \
    -e CONTAINER_IMAGE_ID="$image_id" \
    -e GIT_CONFIG_COUNT=1 \
    -e GIT_CONFIG_KEY_0=safe.directory \
    -e GIT_CONFIG_VALUE_0='*' \
    "$NAME" bash -lc 'export PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.local/bin:/home/xuanwo/.cargo/bin:$PATH; exec bash /evidence/driver/body.sh' \
    2>&1 | tee "$EVID_ROOT/logs/body-$RUN_ID.log"
body_rc=${PIPESTATUS[0]}
set -e

docker exec -u 0 "$NAME" bash -lc 'chown -R 1000:1000 /work /evidence' >/dev/null 2>&1 || true
docker rm -f "$NAME" >/dev/null 2>&1 || true

printf 'body_exit=%s\n' "$body_rc" | tee -a "$EVID_ROOT/driver/run-context.env"
printf 'evidence=%s\n' "$EVID_ROOT"
if [ "$body_rc" -ne 0 ]; then
    printf 'RESULT: FAIL (see %s/commands-status.txt)\n' "$EVID_ROOT" >&2
    exit "$body_rc"
fi
printf 'RESULT: PASS\n'
