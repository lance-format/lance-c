#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Render the public C/C++ API reference and fail when Doxygen reports
# documentation problems.
#
# The header-only self-test first proves that the configuration catches the
# diagnostics this check exists for (wrong @param names, missing parameter
# documentation, malformed markup, and unresolved @ref targets). Then Doxyfile
# renders include/lance/ with warnings treated as errors.
#
# Run .github/scripts/install-doxygen.sh first, or set DOXYGEN_BIN.

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

doxygen_bin="${DOXYGEN_BIN:-}"
if [[ -z "$doxygen_bin" ]]; then
    doxygen_bin="$(command -v doxygen || true)"
fi
if [[ -z "$doxygen_bin" ]]; then
    echo "error: doxygen not found; run .github/scripts/install-doxygen.sh or set DOXYGEN_BIN" >&2
    exit 1
fi

fail() {
    echo "error: $*" >&2
    exit 1
}

self_test_root="$(mktemp -d "${TMPDIR:-/tmp}/lance-c-doxygen-self-test.XXXXXX")"
trap 'rm -rf "$self_test_root"' EXIT

write_self_test_config() {
    local case_dir="$1"
    local input="$2"

    cat >"${case_dir}/Doxyfile" <<EOF
PROJECT_NAME = lance-c-doxygen-self-test
INPUT = ${input}
OUTPUT_DIRECTORY = ${case_dir}/out
GENERATE_HTML = YES
GENERATE_LATEX = NO
QUIET = YES
WARNINGS = YES
WARN_IF_UNDOCUMENTED = NO
WARN_IF_DOC_ERROR = YES
WARN_IF_INCOMPLETE_DOC = YES
WARN_AS_ERROR = FAIL_ON_WARNINGS
WARN_LOGFILE = ${case_dir}/out/warnings.log
EOF
}

# The broken header must make the check fail for the four reasons the CI job
# promises to catch. Checking the messages keeps the self-test from passing on
# an unrelated failure.
mkdir -p "${self_test_root}/broken"
cat >"${self_test_root}/broken/broken.h" <<'EOF'
/** @file broken.h */
/**
 * @brief Write rows to <uri>.
 * @param not_a_parameter This name is not in the signature.
 * @return 0 on success.
 */
int self_test_write(int actual_parameter);

/** @brief Read the context described by @ref missing_symbol. */
void self_test_read(void);
EOF
write_self_test_config "${self_test_root}/broken" "${self_test_root}/broken/broken.h"

if "${doxygen_bin}" "${self_test_root}/broken/Doxyfile" \
    >"${self_test_root}/broken/console.log" 2>&1; then
    fail "the Doxygen self-test accepted a header with broken documentation; warnings-as-errors is not effective"
fi

broken_log="${self_test_root}/broken/out/warnings.log"
if [[ ! -f "$broken_log" ]]; then
    cat "${self_test_root}/broken/console.log" >&2
    fail "the Doxygen self-test did not write ${broken_log}"
fi

for expected in \
    "is not found in the argument list" \
    "is not documented" \
    "Unsupported xml/html tag <uri>" \
    "unable to resolve reference to 'missing_symbol'"; do
    if ! grep -Fq -- "$expected" "$broken_log"; then
        echo "Doxygen self-test log:" >&2
        cat "$broken_log" >&2
        fail "the Doxygen self-test did not report: ${expected}"
    fi
done

# A correct header must still pass, so the check cannot be satisfied by
# failing everything.
mkdir -p "${self_test_root}/clean"
cat >"${self_test_root}/clean/clean.h" <<'EOF'
/** @file clean.h */
/**
 * @brief Write one row.
 * @param uri   Destination URI.
 * @param value Value to write.
 * @return 0 on success, -1 on error.
 */
int self_test_write_clean(const char *uri, int value);
EOF
write_self_test_config "${self_test_root}/clean" "${self_test_root}/clean/clean.h"

if ! "${doxygen_bin}" "${self_test_root}/clean/Doxyfile" \
    >"${self_test_root}/clean/console.log" 2>&1; then
    echo "Doxygen self-test log:" >&2
    cat "${self_test_root}/clean/console.log" >&2
    fail "the Doxygen self-test rejected a correctly documented header"
fi
if [[ ! -f "${self_test_root}/clean/out/html/index.html" ]]; then
    fail "the Doxygen self-test did not render the clean header"
fi

# Render the public headers with the repository configuration.
output_dir="${repo_root}/build/doxygen"
console_log="${output_dir}/doxygen.log"
warnings_log="${output_dir}/warnings.log"
rm -rf "$output_dir"
mkdir -p "$output_dir"

if ! "${doxygen_bin}" Doxyfile >"$console_log" 2>&1; then
    echo "error: Doxygen reported public API documentation problems" >&2
    if [[ -s "$warnings_log" ]]; then
        cat "$warnings_log" >&2
    else
        cat "$console_log" >&2
    fi
    exit 1
fi

html_dir="${output_dir}/html"
for required in \
    index.html \
    lance_8h.html \
    lance_8hpp.html \
    version_8h.html \
    error_8h.html \
    arrow__abi_8h.html \
    fwd_8h.html \
    dataset_8h.html \
    scanner_8h.html \
    batch_8h.html \
    attributes_8h.html \
    declarations_8hpp.html \
    classlance_1_1Dataset.html \
    classlance_1_1Scanner.html \
    search/searchdata.js; do
    [[ -e "${html_dir}/${required}" ]] ||
        fail "rendered API reference is missing ${required}; check INPUT and EXTRACT_ALL in Doxyfile"
done

# Moved declarations must be documented on their domain page, and declarations
# that stayed in the umbrella must still be documented there.
for pair in \
    "dataset_8h.html lance_dataset_open" \
    "scanner_8h.html lance_scanner_next" \
    "error_8h.html lance_last_error_code" \
    "version_8h.html LANCE_C_VERSION" \
    "batch_8h.html lance_batch_to_arrow" \
    "arrow__abi_8h.html ArrowArrayStream" \
    "fwd_8h.html LanceDataset" \
    "lance_8h.html lance_dataset_compact_files"; do
    page=${pair%% *}
    symbol=${pair#* }
    grep -Fq -- "$symbol" "${html_dir}/${page}" ||
        fail "rendered API reference does not mention ${symbol} in ${page}"
done
for symbol in 'lance::Dataset' 'lance::Error'; do
    grep -Fq -- "$symbol" "${html_dir}/lance_8hpp.html" ||
        fail "rendered C++ API reference does not mention ${symbol}"
done

page_count="$(find "$html_dir" -name '*.html' | wc -l | tr -d ' ')"
echo "Doxygen: $("$doxygen_bin" --version)"
echo "API reference: ${page_count} HTML pages, $(du -sh "$html_dir" | cut -f1) in ${html_dir}"
echo "Validated the public C headers, include/lance/lance.hpp and include/lance/detail/declarations.hpp with no Doxygen warnings."
