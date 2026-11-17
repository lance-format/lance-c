#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Host driver for probe.sh: one pristine container from the same digest-pinned
# image, the extracted archive mounted read-only as the consumer prefix.
#
# Usage: SOURCE=<product clone> EVID_ROOT=<evidence dir containing work/extracted> \
#        bash run-probe.sh
set -euo pipefail
export LC_ALL=C

EVID=${EVID_ROOT:?set EVID_ROOT}
SOURCE=${SOURCE:?set SOURCE}
IMAGE=quay.io/pypa/manylinux_2_28_x86_64@sha256:d086f64cddcace250ef0f0073583c51b8298159303290a730c9f59bd91edfd2c
NAME=lance-c-pr50-supplement
OUT=$EVID/supplement

[ -d "$EVID/work/extracted/lib" ] || { echo "missing $EVID/work/extracted" >&2; exit 1; }
head=$(git -C "$SOURCE" rev-parse HEAD)
[ "$head" = 26cde77e8c1acc538ec1a2f3be26f8eeea74373a ] || {
    echo "product clone is at $head, not the expected head" >&2
    exit 1
}
mkdir -p "$OUT/logs"
cp "$(dirname "$0")/probe.sh" "$OUT/probe.sh"
CLANG_VERSION=$(grep -oE 'manylinux-install-clang -v [^ ]+' "$SOURCE/.github/workflows/release.yml" |
    head -1 | awk '{print $NF}')

docker pull "$IMAGE" >/dev/null
docker rm -f "$NAME" >/dev/null 2>&1 || true
docker run -d --name "$NAME" --platform linux/amd64 \
    -v "$SOURCE:/source:ro" \
    -v "$EVID/work:/work" \
    -v "$EVID/supplement:/evidence" \
    -v "$HOME/.local/bin/mbx:/home/xuanwo/.local/bin/mbx:ro" \
    -v "$HOME/.local/share/mbx:/home/xuanwo/.local/share/mbx:ro" \
    -v "$HOME/.cargo:/home/xuanwo/.cargo" \
    -v "$HOME/.cache/mbx:/home/xuanwo/.cache/mbx" \
    -e CARGO_HOME=/home/xuanwo/.cargo \
    -e CARGO_BUILD_JOBS=8 \
    -e CARGO_TERM_COLOR=never \
    -e RUSTUP_TOOLCHAIN=stable \
    "$IMAGE" bash -lc 'trap "exit 0" TERM INT; sleep infinity & wait' >/dev/null

cleanup() { docker rm -f "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker exec -u 0 "$NAME" bash -lc "
    set -e
    mkdir -p /home/xuanwo /evidence/logs
    chown 1000:1000 /home/xuanwo /opt /usr/local/bin /usr/local/include
    chown -R 1000:1000 /evidence
" >/dev/null

docker exec -u 0 "$NAME" bash -lc "manylinux-install-clang -v $CLANG_VERSION" \
    >"$OUT/logs/clang-install.log" 2>&1 || { echo "clang install failed" >&2; exit 1; }

set +e
docker exec --user 1000:1000 "$NAME" bash -lc '
    export HOME=/home/xuanwo
    export PATH=/home/xuanwo/.local/share/mbx/bin:/home/xuanwo/.local/bin:/home/xuanwo/.cargo/bin:$PATH
    export CC=clang CXX=clang++
    exec bash /evidence/probe.sh
' 2>&1 | tee "$OUT/logs/probe-console.log"
rc=${PIPESTATUS[0]}
set -e
docker exec -u 0 "$NAME" chown -R 1000:1000 /evidence >/dev/null 2>&1 || true
echo "probe_exit=$rc"
exit "$rc"
