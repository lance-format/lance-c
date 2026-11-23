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
#     4. Build and run pure-C and C++17 C API consumers via find_package and
#        add_subdirectory, then `examples/cmake-consumer` from the source tree
#        and from a copied-out directory against the installed prefix, with both static
#        and shared linkage. This includes the write/index/query workflow.
#     5. Build and run `examples/c-scan` through pkg-config with both dynamic and
#        static linkage.
#
#   verify-cmake-consumer.sh --from-install-prefix <install-prefix>
#     Build and run the same consumers against an already-installed prefix
#     (e.g. `stage` in release.yml).

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

pkg_config_command() {
    if command -v pkg-config >/dev/null 2>&1; then
        echo pkg-config
    elif command -v pkgconf >/dev/null 2>&1; then
        echo pkgconf
    else
        echo "pkg-config or pkgconf is required for installed-package verification" >&2
        return 1
    fi
}

verify_pkg_config_consumer() {
    local install_prefix=$1
    local pc_dir="$install_prefix/lib/pkgconfig"
    local dataset_uri="$repo_root/test_data/v0.27.1/pq_in_schema"
    local expected_dataset_output="rows: 512, version: 2"
    local cc=${CC:-cc}
    local pkg_config
    pkg_config=$(pkg_config_command)

    echo "==> Verifying pkg-config and C scan consumer"
    local pkg_config_path="$pc_dir"
    if [ -n "${PKG_CONFIG_PATH:-}" ]; then
        pkg_config_path="$pkg_config_path:$PKG_CONFIG_PATH"
    fi

    local cflags libs
    cflags=$(PKG_CONFIG_PATH="$pkg_config_path" "$pkg_config" --cflags lance-c)
    libs=$(PKG_CONFIG_PATH="$pkg_config_path" "$pkg_config" --cflags --libs lance-c)
    printf 'pkg-config cflags: %s\n' "$cflags"
    printf 'pkg-config libs:   %s\n' "$libs"
    if [[ " $libs " != *" -llance_c "* ]]; then
        echo "pkg-config libs do not link lance_c: $libs" >&2
        exit 1
    fi

    local consumer_dir="$work_dir/pkg-config-consumer"
    mkdir -p "$consumer_dir"

    # shellcheck disable=SC2086 # pkg-config output is a list of linker flags.
    "$cc" -std=c11 -O2 $cflags "$repo_root/examples/c-scan/main.c" $libs \
        -Wl,-rpath,"$install_prefix/lib" \
        -o "$consumer_dir/lance-c-scan-shared"

    local dataset_output
    dataset_output=$("$consumer_dir/lance-c-scan-shared" "$dataset_uri")
    if [ "$dataset_output" != "$expected_dataset_output" ]; then
        echo "pkg-config dynamic C consumer returned unexpected output:" >&2
        printf '  expected: %s\n  actual:   %s\n' \
            "$expected_dataset_output" "$dataset_output" >&2
        exit 1
    fi
    printf 'Verified pkg-config dynamic C consumer dataset read: %s\n' \
        "$dataset_output"

    local static_libs static_link_flags
    static_libs=$(PKG_CONFIG_PATH="$pkg_config_path" "$pkg_config" \
        --static --libs-only-l lance-c)
    static_link_flags=$(PKG_CONFIG_PATH="$pkg_config_path" "$pkg_config" \
        --static --libs-only-other lance-c)
    static_libs=${static_libs//-llance_c/}

    # Link the static archive explicitly and take the system libraries from
    # pkg-config's Libs.private.
    # shellcheck disable=SC2086 # pkg-config output is a list of linker flags.
    "$cc" -std=c11 -O2 $cflags "$repo_root/examples/c-scan/main.c" \
        "$install_prefix/lib/liblance_c.a" $static_libs $static_link_flags \
        -o "$consumer_dir/lance-c-scan-static"

    dataset_output=$("$consumer_dir/lance-c-scan-static" "$dataset_uri")
    if [ "$dataset_output" != "$expected_dataset_output" ]; then
        echo "pkg-config static C consumer returned unexpected output:" >&2
        printf '  expected: %s\n  actual:   %s\n' \
            "$expected_dataset_output" "$dataset_output" >&2
        exit 1
    fi
    printf 'Verified pkg-config static C consumer dataset read: %s\n' \
        "$dataset_output"
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

    echo "==> Verifying installed package metadata"
    cmake "-DPREFIX=$install_prefix" "-DPLATFORM=$(uname -s)" \
        -P "$repo_root/cmake/verify_installed_metadata.cmake"

    verify_pkg_config_consumer "$install_prefix"

    # Exercise project(... LANGUAGES C) and a C++17 application that includes
    # only lance.h. The existing examples below exercise the C++20 wrapper.
    for integration in installed subdirectory; do
        for linkage in static shared; do
            for language in C CXX; do
                local api_build="$work_dir/c-api-${integration}-${linkage}-${language}"
                echo "==> Testing ${language} C API consumer (${integration}, ${linkage})"
                cmake -S "$repo_root/cmake/tests/target-consumer" -B "$api_build" \
                    -DTEST_REPO="$repo_root" -DTEST_PREFIX="$install_prefix" \
                    -DCMAKE_PREFIX_PATH="$install_prefix" \
                    -DTEST_MODE="$integration" -DTEST_LANGUAGE="$language" \
                    -DTEST_LINK_CONSUMER=ON -DLanceC_LINK="$linkage" \
                    -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
                cmake --build "$api_build" --parallel
                "$api_build/consumer"
            done
        done
    done

    local dataset_uri="$repo_root/test_data/v0.27.1/pq_in_schema"
    local expected_dataset_output="rows: 512, version: 2"

    for source_layout in source standalone; do
        local consumer_source="$repo_root/examples/cmake-consumer"
        if [ "$source_layout" = standalone ]; then
            consumer_source="$work_dir/standalone-consumer"
            rm -rf "$consumer_source"
            cp -R "$repo_root/examples/cmake-consumer" "$consumer_source"
        fi

        for linkage in static shared; do
            local build_dir="$work_dir/consumer-${source_layout}-${linkage}"
            echo "==> Testing find_package(LanceC) ${source_layout} consumer (${linkage} linkage)"
            cmake -S "$consumer_source" -B "$build_dir" \
                -DCMAKE_PREFIX_PATH="$install_prefix" \
                -DCMAKE_BUILD_TYPE=Release \
                -DLanceC_LINK="$linkage"
            cmake --build "$build_dir" --parallel

            set +e
            "$build_dir/consumer"
            local rc=$?
            set -e
            if [ "$rc" -ne 2 ]; then
                echo "Expected exit code 2 (usage error) for ${source_layout} ${linkage} consumer, got $rc" >&2
                exit 1
            fi

            set +e
            local dataset_output
            dataset_output=$("$build_dir/consumer" "$dataset_uri" 2>&1)
            rc=$?
            set -e
            if [ "$rc" -ne 0 ]; then
                echo "${source_layout} ${linkage} consumer failed to read $dataset_uri (exit $rc):" >&2
                printf '%s\n' "$dataset_output" >&2
                exit 1
            fi
            if [ "$dataset_output" != "$expected_dataset_output" ]; then
                echo "${source_layout} ${linkage} consumer returned unexpected dataset output:" >&2
                printf '  expected: %s\n  actual:   %s\n' \
                    "$expected_dataset_output" "$dataset_output" >&2
                exit 1
            fi
            printf 'Verified %s %s consumer dataset read: %s\n' \
                "$source_layout" "$linkage" "$dataset_output"

            echo "==> Running installed acceptance consumer (${source_layout} ${linkage} linkage)"
            acceptance_uri="file://${work_dir}/acceptance-${source_layout}-${linkage}.lance"
            rm -rf "${work_dir}/acceptance-${source_layout}-${linkage}.lance"
            "$build_dir/acceptance" "$acceptance_uri"

            echo "==> Running installed write/index/query workflow (${source_layout} ${linkage} linkage)"
            workflow_uri="file://${work_dir}/workflow-${source_layout}-${linkage}.lance"
            rm -rf "${work_dir}/workflow-${source_layout}-${linkage}.lance"
            "$build_dir/workflow" "$workflow_uri"
        done
    done
}

echo "==> Verifying platform dependency metadata"
cmake -P "$repo_root/cmake/test_package_metadata.cmake"

case "$mode" in
    --from-target-dir)
        target_dir=$(cd "$arg" && pwd)

        echo "==> Validating CMake + Corrosion configure/generate"
        cmake -S "$repo_root" -B "$work_dir/build-corrosion" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_LIBDIR=lib
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
            -DCMAKE_INSTALL_LIBDIR=lib \
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
