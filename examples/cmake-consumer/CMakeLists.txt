# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)
project(lance_c_consumer LANGUAGES CXX)

# Cargo.toml in this checkout is the source of truth for the package version.
# Read it so this example requests the installed package built from the same
# tree, rather than pinning a minor version that a later release outgrows.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(cargo-version)
read_cargo_version("${CMAKE_CURRENT_LIST_DIR}/../../Cargo.toml" LANCE_C_CONSUMER_VERSION)

find_package(LanceC ${LANCE_C_CONSUMER_VERSION} REQUIRED)

# The Windows release archive is Release /MT. Select the matching consumer CRT
# explicitly; an imported library cannot propagate CMAKE_MSVC_RUNTIME_LIBRARY.
if(WIN32 AND LanceC_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE LanceC::lance_c)

add_executable(acceptance acceptance.cpp)
target_link_libraries(acceptance PRIVATE LanceC::lance_c)
