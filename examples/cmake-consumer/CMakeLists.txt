# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

cmake_minimum_required(VERSION 3.22)
project(lance_c_consumer LANGUAGES CXX)

# In the source tree, Cargo.toml is the source of truth for the package
# version. Read it so this example requests the package built from the same
# tree, rather than pinning a minor version that a later release outgrows.
# A copy outside the repository has neither file and accepts the installed
# package without a version request.
set(_lance_c_source_root "${CMAKE_CURRENT_LIST_DIR}/../..")
if(EXISTS "${_lance_c_source_root}/Cargo.toml"
        AND EXISTS "${_lance_c_source_root}/cmake/cargo-version.cmake")
    list(PREPEND CMAKE_MODULE_PATH "${_lance_c_source_root}/cmake")
    include(cargo-version)
    read_cargo_version(
        "${_lance_c_source_root}/Cargo.toml" LANCE_C_CONSUMER_VERSION)
    message(STATUS
        "LanceC consumer: source tree, requesting ${LANCE_C_CONSUMER_VERSION}")
    find_package(LanceC ${LANCE_C_CONSUMER_VERSION} REQUIRED)
else()
    message(STATUS "LanceC consumer: standalone, accepting the installed package")
    find_package(LanceC CONFIG REQUIRED)
endif()

# The Windows release archive is Release /MT. Select the matching consumer CRT
# explicitly; an imported library cannot propagate CMAKE_MSVC_RUNTIME_LIBRARY.
if(WIN32 AND LanceC_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE LanceC::lance_cpp)

add_executable(acceptance acceptance.cpp)
target_link_libraries(acceptance PRIVATE LanceC::lance_cpp)

add_executable(workflow workflow.cpp)
target_link_libraries(workflow PRIVATE LanceC::lance_cpp)
