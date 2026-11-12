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
endif()

set(PROBE_CONSUMER "shared" CACHE STRING "shared, static_md, or static_mt")

if(PROBE_CONSUMER STREQUAL "shared")
    add_executable(probe shared.cpp)
    target_link_libraries(probe PRIVATE crt_probe-shared)
elseif(PROBE_CONSUMER STREQUAL "static_md")
    add_executable(probe static.cpp)
    target_link_libraries(probe PRIVATE crt_probe-static)
    set_property(TARGET probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
elseif(PROBE_CONSUMER STREQUAL "static_mt")
    add_executable(probe static.cpp)
    target_link_libraries(probe PRIVATE crt_probe-static)
    set_property(TARGET probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
else()
    message(FATAL_ERROR "Unknown PROBE_CONSUMER=${PROBE_CONSUMER}")
endif()

