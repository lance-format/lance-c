# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify that examples/cmake-consumer requests the package version built from
# its own source tree and that installed packages keep enforcing
# SameMinorVersion. No binaries are built.
#
# Run with: cmake -P cmake/test_consumer_version.cmake

cmake_minimum_required(VERSION 3.22)

include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/platform-deps.cmake")

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_work_dir "${CMAKE_CURRENT_LIST_DIR}/.test_consumer_version_tmp")
file(REMOVE_RECURSE "${_work_dir}")
file(MAKE_DIRECTORY "${_work_dir}")

lance_c_platform_dependencies(_test_platform_deps "${CMAKE_SYSTEM_NAME}")

function(_write_package prefix version)
    file(MAKE_DIRECTORY "${prefix}/include/lance" "${prefix}/lib/cmake/LanceC")
    file(WRITE "${prefix}/include/lance/lance.h" "")
    # The installed config checks for the platform-specific library name.
    file(WRITE "${prefix}/lib/liblance_c.a" "")
    file(WRITE "${prefix}/lib/lance_c.lib" "")

    set(CMAKE_INSTALL_INCLUDEDIR include)
    set(CMAKE_INSTALL_LIBDIR lib)
    set(LanceC_STATIC_CRT OFF)
    set(LANCE_C_PLATFORM_DEPS "${_test_platform_deps}")
    configure_package_config_file(
        "${_repo_root}/cmake/LanceCConfig.cmake.in"
        "${prefix}/lib/cmake/LanceC/LanceCConfig.cmake"
        INSTALL_DESTINATION lib/cmake/LanceC
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
    write_basic_package_version_file(
        "${prefix}/lib/cmake/LanceC/LanceCConfigVersion.cmake"
        VERSION "${version}"
        COMPATIBILITY SameMinorVersion)
endfunction()

function(_write_source_tree source version)
    file(MAKE_DIRECTORY "${source}/cmake" "${source}/examples")
    file(COPY "${_repo_root}/examples/cmake-consumer"
        DESTINATION "${source}/examples")
    file(COPY "${_repo_root}/cmake/cargo-version.cmake"
        DESTINATION "${source}/cmake")
    file(WRITE "${source}/Cargo.toml"
        "[package]\nname = \"lance-c\"\nversion = \"${version}\"\n")
endfunction()

# A copied-out example has no Cargo.toml or cmake/ helper directory beside it.
function(_write_standalone_tree source)
    file(MAKE_DIRECTORY "${source}/examples")
    file(COPY "${_repo_root}/examples/cmake-consumer"
        DESTINATION "${source}/examples")
endfunction()

function(_configure_consumer source prefix build result_var output_var)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${source}/examples/cmake-consumer"
            -B "${build}"
            "-DCMAKE_PREFIX_PATH=${prefix}"
            -DCMAKE_BUILD_TYPE=Release
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(${result_var} "${_status}" PARENT_SCOPE)
    set(${output_var} "${_stdout}\n${_stderr}" PARENT_SCOPE)
endfunction()

_write_package("${_work_dir}/package-v0.2" "0.2.0")
_write_source_tree("${_work_dir}/source-v0.2" "0.2.0-beta.1")
_write_source_tree("${_work_dir}/source-v0.1" "0.1.9")
_write_standalone_tree("${_work_dir}/standalone-v0.2")

# The consumer from the 0.2 source tree must select the 0.2 package.
_configure_consumer("${_work_dir}/source-v0.2" "${_work_dir}/package-v0.2"
    "${_work_dir}/build-v0.2" _status _output)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "0.2 consumer did not select the 0.2 package:\n${_output}")
endif()
if(NOT _output MATCHES "LanceC consumer: source tree")
    message(FATAL_ERROR "In-tree consumer did not use the source-tree version:\n${_output}")
endif()

# A copied-out consumer must work without the repository layout and accept the
# installed package without a version request.
_configure_consumer("${_work_dir}/standalone-v0.2" "${_work_dir}/package-v0.2"
    "${_work_dir}/build-standalone-v0.2" _status _output)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR "Standalone consumer did not select the 0.2 package:\n${_output}")
endif()
if(NOT _output MATCHES "LanceC consumer: standalone")
    message(FATAL_ERROR "Standalone consumer did not use the unversioned fallback:\n${_output}")
endif()

# SameMinorVersion must still reject a 0.1 request against a 0.2 package.
_configure_consumer("${_work_dir}/source-v0.1" "${_work_dir}/package-v0.2"
    "${_work_dir}/build-v0.1" _status _output)
if(_status EQUAL 0)
    message(FATAL_ERROR "0.1 consumer unexpectedly selected the 0.2 package")
endif()
if(NOT _output MATCHES "requested version \"0\\.1")
    message(FATAL_ERROR "0.1 request failed for an unexpected reason:\n${_output}")
endif()

file(REMOVE_RECURSE "${_work_dir}")
message(STATUS "All consumer version tests passed.")
