#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Validate LanceC's CMake package layout and consumer linkage (`static` + `shared`).
#
# Modes:
#   verify-cmake-consumer.sh --from-target-dir <cargo-profile-dir>
#     1. Configure the default Corrosion build tree (without compiling Rust) to
#        exercise FetchContent(Corrosion), corrosion_import_crate(), and install
#        generator expressions.
#     2. Stage <cargo-profile-dir>/liblance_c.* via LANCE_C_USE_PREBUILT=ON and
#        run `cmake --install` into a clean prefix.
#     3. Verify macOS Mach-O architectures when running on Darwin.
#     4. Build and run `examples/cmake-consumer` against the installed prefix
#        with both static and shared linkage.
#
#   verify-cmake-consumer.sh --from-install-prefix <install-prefix>
#     Build and run `examples/cmake-consumer` against an already-installed
#     prefix (e.g. `stage` in release.yml) with both static and shared linkage.

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 (--from-target-dir <dir> | --from-install-prefix <prefix>)" >&2
    exit 2
fi

mode=$1
arg=$2

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lance-c-cmake-smoke.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

verify_corrosion_macos_linker() {
    local build_dir=$1

    if [ "$(uname -s)" != Darwin ]; then
        return
    fi

    echo "==> Verifying Corrosion selects the macOS export linker"
    local build_commands
    build_commands=$(cmake --build "$build_dir" \
        --target _cargo-build_lance_c -- -n 2>&1)
    local linker_env
    linker_env=$(printf '%s\n' "$build_commands" |
        tr ' ' '\n' |
        grep -E '^(CARGO_TARGET_[A-Z0-9_]+_LINKER|LANCE_C_MACOS_LINKER)=' || true)
    if ! grep -Fq 'limit-macos-exports.sh' <<<"$linker_env" ||
        ! grep -Fq 'LANCE_C_MACOS_LINKER=' <<<"$linker_env"; then
        echo "Corrosion did not select the macOS export linker:" >&2
        printf '%s\n' "$linker_env" >&2
        exit 1
    fi
    printf '%s\n' "$linker_env"
}

verify_consumer_prefix() {
    local install_prefix=$1

    echo "==> Verifying installed artifacts in $install_prefix"
    case "$(uname -s)" in
        Darwin)
            bash "$repo_root/.github/scripts/verify-macos-staging.sh" \
                "$install_prefix" "$(uname -m)"
            bash "$repo_root/.github/scripts/verify-shared-exports.sh" \
                "$install_prefix/lib/liblance_c.dylib" \
                "$install_prefix/include/lance/lance.h" macos
            ;;
        Linux)
            bash "$repo_root/.github/scripts/verify-shared-exports.sh" \
                "$install_prefix/lib/liblance_c.so" \
                "$install_prefix/include/lance/lance.h" linux
            ;;
        *)
            echo "unsupported platform: $(uname -s)" >&2
            exit 1
            ;;
    esac

    echo "==> Installed library identity"
    for library in \
        "$install_prefix/lib/liblance_c.a" \
        "$install_prefix/lib/liblance_c.so" \
        "$install_prefix/lib/liblance_c.dylib"; do
        if [ -f "$library" ]; then
            if command -v file >/dev/null 2>&1; then
                file "$library"
            fi
            if command -v sha256sum >/dev/null 2>&1; then
                sha256sum "$library"
            else
                shasum -a 256 "$library"
            fi
            wc -c "$library"
        fi
    done

    local dataset_uri="$repo_root/test_data/v0.27.1/pq_in_schema"
    local expected_dataset_output="rows: 512, version: 2"

    for linkage in static shared; do
        local build_dir="$work_dir/consumer-${linkage}"
        echo "==> Testing find_package(LanceC) consumer (${linkage} linkage)"
        cmake -S "$repo_root/examples/cmake-consumer" -B "$build_dir" \
            -DCMAKE_PREFIX_PATH="$install_prefix" \
            -DCMAKE_BUILD_TYPE=Release \
            -DLanceC_LINK="$linkage"
        cmake --build "$build_dir" --parallel

        set +e
        "$build_dir/consumer"
        local rc=$?
        set -e
        if [ "$rc" -ne 2 ]; then
            echo "Expected exit code 2 (usage error) for ${linkage} consumer, got $rc" >&2
            exit 1
        fi

        set +e
        local dataset_output
        dataset_output=$("$build_dir/consumer" "$dataset_uri" 2>&1)
        rc=$?
        set -e
        if [ "$rc" -ne 0 ]; then
            echo "${linkage} consumer failed to read $dataset_uri (exit $rc):" >&2
            printf '%s\n' "$dataset_output" >&2
            exit 1
        fi
        if [ "$dataset_output" != "$expected_dataset_output" ]; then
            echo "${linkage} consumer returned unexpected dataset output:" >&2
            printf '  expected: %s\n  actual:   %s\n' \
                "$expected_dataset_output" "$dataset_output" >&2
            exit 1
        fi
        printf 'Verified %s consumer dataset read: %s\n' "$linkage" "$dataset_output"

        echo "==> Running installed acceptance consumer (${linkage} linkage)"
        acceptance_uri="file://${work_dir}/acceptance-${linkage}.lance"
        rm -rf "${work_dir}/acceptance-${linkage}.lance"
        "$build_dir/acceptance" "$acceptance_uri"
    done
}

case "$mode" in
    --from-target-dir)
        target_dir=$(cd "$arg" && pwd)

        echo "==> Validating CMake + Corrosion configure/generate"
        cmake -S "$repo_root" -B "$work_dir/build-corrosion" \
            -DCMAKE_BUILD_TYPE=Release
        verify_corrosion_macos_linker "$work_dir/build-corrosion"

        echo "==> Staging prebuilt artifacts from $target_dir and running cmake --install"
        prebuilt_dir="$work_dir/prebuilt"
        mkdir -p "$prebuilt_dir/lib"
        ln -s "$repo_root/include" "$prebuilt_dir/include"
        for candidate in \
            "$target_dir/liblance_c.a" \
            "$target_dir/liblance_c.so" \
            "$target_dir/liblance_c.dylib"; do
            if [ -f "$candidate" ]; then
                ln -s "$candidate" "$prebuilt_dir/lib/$(basename "$candidate")"
            fi
        done

        install_dir="$work_dir/install"
        cmake -S "$repo_root" -B "$work_dir/build-prebuilt" \
            -DCMAKE_BUILD_TYPE=Release \
            -DLANCE_C_USE_PREBUILT=ON \
            -DLANCE_C_PREBUILT_DIR="$prebuilt_dir"
        cmake --install "$work_dir/build-prebuilt" --prefix "$install_dir"

        verify_consumer_prefix "$install_dir"
        ;;
    --from-install-prefix)
        install_dir=$(cd "$arg" && pwd)
        verify_consumer_prefix "$install_dir"
        ;;
    *)
        echo "unknown mode: $mode" >&2
        exit 2
        ;;
esac
