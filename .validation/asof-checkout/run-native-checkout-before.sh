#!/usr/bin/env bash
set -euo pipefail

BASE_SHA=40f15ed1b71b66950961b175829fdf17a763f108
EXPECTED_TESTS=7
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
PATCH="$SCRIPT_DIR/native-checkout-before.patch"

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/disposable/lance-checkout-at-${BASE_SHA}" >&2
    exit 2
fi

LANCE_DIR=$(cd -- "$1" && pwd)
ACTUAL_SHA=$(git -C "$LANCE_DIR" rev-parse HEAD)
if [[ "$ACTUAL_SHA" != "$BASE_SHA" ]]; then
    echo "error: expected base $BASE_SHA, found $ACTUAL_SHA in $LANCE_DIR" >&2
    exit 1
fi
if [[ -n "$(git -C "$LANCE_DIR" status --porcelain)" ]]; then
    echo "error: $LANCE_DIR is not clean; use a disposable checkout" >&2
    exit 1
fi

RUSTC_VERSION=$(rustc +1.94.0 --version | awk '{print $2}')
if [[ "$RUSTC_VERSION" != "1.94.0" ]]; then
    echo "error: expected rustc 1.94.0, found $RUSTC_VERSION" >&2
    exit 1
fi

git -C "$LANCE_DIR" apply --check "$PATCH"
git -C "$LANCE_DIR" apply "$PATCH"

LOG=$(mktemp "${TMPDIR:-/tmp}/asof-checkout-before.XXXXXX.log")
(
    cd "$LANCE_DIR"
    cargo +1.94.0 fmt --all -- --check
    cargo +1.94.0 test -j 8 -p lance --lib checkout_before -- --test-threads=4
) 2>&1 | tee "$LOG"

if ! grep -Eq "^running ${EXPECTED_TESTS} tests$" "$LOG"; then
    echo "error: expected ${EXPECTED_TESTS} focused tests, see $LOG" >&2
    exit 1
fi
if ! grep -Eq "^test result: ok\\. ${EXPECTED_TESTS} passed" "$LOG"; then
    echo "error: expected ${EXPECTED_TESTS} passing tests, see $LOG" >&2
    exit 1
fi

echo "focused validation passed; log: $LOG"
