# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)

project(CrtStaticProbe LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.5.1
)
FetchContent_MakeAvailable(Corrosion)

corrosion_import_crate(
    MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
    CRATES crt_probe
    CRATE_TYPES staticlib cdylib
    PROFILE release
)

if(Rust_CARGO_TARGET_ENV STREQUAL "msvc")
    corrosion_add_target_rustflags(
        crt_probe
        "-Ctarget-feature=+crt-static"
    )

    # Corrosion probes native-static-libs without the target RUSTFLAGS, so its
    # static target advertises the dynamic CRT by default. The target archive
    # itself is built with crt-static and carries LIBCMT directives.
    get_target_property(_crt_probe_link_options crt_probe-static
        INTERFACE_LINK_OPTIONS)
    message(STATUS "crt_probe-static INTERFACE_LINK_OPTIONS before: ${_crt_probe_link_options}")
    list(FILTER _crt_probe_link_options
        EXCLUDE REGEX "^/defaultlib:[mM][sS][vV][cC][rR][tT][dD]?$")
    set_target_properties(crt_probe-static PROPERTIES
        INTERFACE_LINK_OPTIONS "${_crt_probe_link_options}")
    message(STATUS "crt_probe-static INTERFACE_LINK_OPTIONS after: ${_crt_probe_link_options}")
endif()

add_executable(probe static.cpp)
target_link_libraries(probe PRIVATE crt_probe-static)
set_property(TARGET probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
