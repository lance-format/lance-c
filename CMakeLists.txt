# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(cargo-version)
read_cargo_version("${CMAKE_CURRENT_LIST_DIR}/Cargo.toml" LANCE_C_VERSION)

project(LanceC
    VERSION ${LANCE_C_VERSION}
    DESCRIPTION "C/C++ bindings for the Lance columnar data format"
    HOMEPAGE_URL "https://github.com/lance-format/lance-c"
    LANGUAGES C CXX
)

include(GNUInstallDirs)

# ─── Options ─────────────────────────────────────────────────────────────────
option(LANCE_C_USE_PREBUILT
    "Skip cargo build and use a prebuilt liblance_c at LANCE_C_PREBUILT_DIR"
    OFF)
set(LANCE_C_PREBUILT_DIR "" CACHE PATH
    "Directory containing prebuilt liblance_c.{a,so,dylib} (when LANCE_C_USE_PREBUILT=ON)")
set(LANCE_C_LINK "static" CACHE STRING
    "Default linkage for the LanceC::lance_c alias: static or shared")
set_property(CACHE LANCE_C_LINK PROPERTY STRINGS static shared)

# ─── Build via Corrosion (or skip for prebuilt mode) ─────────────────────────
if(NOT LANCE_C_USE_PREBUILT)
    include(FetchContent)
    FetchContent_Declare(
        Corrosion
        GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
        GIT_TAG v0.5.1
    )
    FetchContent_MakeAvailable(Corrosion)

    corrosion_import_crate(
        MANIFEST_PATH "${CMAKE_CURRENT_LIST_DIR}/Cargo.toml"
        CRATES lance-c
        CRATE_TYPES staticlib cdylib
        PROFILE release
    )
endif()

# Imported targets and install rules are added in subsequent tasks.
