# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Verify the generated metadata in an install prefix. The CMake package and
# pkg-config file must describe the same platform dependencies, and those
# dependencies must match the platform list in cmake/platform-deps.cmake.
#
# Run with:
#   cmake -DPREFIX=<install-prefix> [-DPLATFORM=<CMAKE_SYSTEM_NAME>] \
#     -P cmake/verify_installed_metadata.cmake

cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED PREFIX OR PREFIX STREQUAL "")
    message(FATAL_ERROR
        "Usage: cmake -DPREFIX=<install-prefix> "
        "[-DPLATFORM=<CMAKE_SYSTEM_NAME>] -P verify_installed_metadata.cmake")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/platform-deps.cmake")

get_filename_component(_prefix "${PREFIX}" ABSOLUTE)
set(_config_file "${_prefix}/lib/cmake/LanceC/LanceCConfig.cmake")
set(_version_file "${_prefix}/lib/cmake/LanceC/LanceCConfigVersion.cmake")
set(_pc_file "${_prefix}/lib/pkgconfig/lance-c.pc")

foreach(_required IN ITEMS "${_config_file}" "${_version_file}" "${_pc_file}")
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "installed metadata is missing ${_required}")
    endif()
endforeach()

file(READ "${_config_file}" _config)
if(NOT _config MATCHES
        "set\\(_lance_c_platform_deps \"([^\"]*)\"\\)")
    message(FATAL_ERROR
        "LanceCConfig.cmake does not declare _lance_c_platform_deps")
endif()
set(_exported_deps "${CMAKE_MATCH_1}")

file(STRINGS "${_pc_file}" _pc_private_line REGEX "^Libs\\.private: ")
if(NOT _pc_private_line)
    message(FATAL_ERROR "lance-c.pc does not declare Libs.private")
endif()
list(GET _pc_private_line 0 _pc_private_value)
string(REGEX REPLACE "^Libs\\.private:[ \t]*" ""
    _pc_private_libs "${_pc_private_value}")
string(STRIP "${_pc_private_libs}" _pc_private_libs)

lance_c_deps_to_pkg_config(_expected_pc_private_libs "${_exported_deps}")
if(NOT "${_pc_private_libs}" STREQUAL "${_expected_pc_private_libs}")
    message(FATAL_ERROR
        "pkg-config Libs.private does not match LanceCConfig.cmake:\n"
        "  CMake dependencies: ${_exported_deps}\n"
        "  expected Libs.private: ${_expected_pc_private_libs}\n"
        "  actual Libs.private:   ${_pc_private_libs}")
endif()

if(DEFINED PLATFORM AND NOT PLATFORM STREQUAL "")
    lance_c_platform_dependencies(_platform_deps "${PLATFORM}")
    if(NOT "${_exported_deps}" STREQUAL "${_platform_deps}")
        message(FATAL_ERROR
            "LanceCConfig.cmake dependencies do not match ${PLATFORM}:\n"
            "  expected: ${_platform_deps}\n"
            "  actual:   ${_exported_deps}")
    endif()

    lance_c_pkg_config_private_libs(_platform_pc_libs "${PLATFORM}")
    if(NOT "${_pc_private_libs}" STREQUAL "${_platform_pc_libs}")
        message(FATAL_ERROR
            "pkg-config Libs.private does not match ${PLATFORM}:\n"
            "  expected: ${_platform_pc_libs}\n"
            "  actual:   ${_pc_private_libs}")
    endif()
endif()

file(STRINGS "${_pc_file}" _pc_libs_line REGEX "^Libs: ")
if(NOT _pc_libs_line)
    message(FATAL_ERROR "lance-c.pc does not declare Libs")
endif()
list(GET _pc_libs_line 0 _pc_libs_value)
if(NOT _pc_libs_value MATCHES "-llance_c")
    message(FATAL_ERROR "lance-c.pc Libs does not link lance_c")
endif()

file(STRINGS "${_pc_file}" _pc_cflags_line REGEX "^Cflags: ")
if(NOT _pc_cflags_line)
    message(FATAL_ERROR "lance-c.pc does not declare Cflags")
endif()
list(GET _pc_cflags_line 0 _pc_cflags_value)
if(NOT _pc_cflags_value MATCHES "-I")
    message(FATAL_ERROR "lance-c.pc Cflags does not expose the include directory")
endif()

file(STRINGS "${_pc_file}" _pc_version_line REGEX "^Version: ")
if(NOT _pc_version_line)
    message(FATAL_ERROR "lance-c.pc does not declare a version")
endif()
list(GET _pc_version_line 0 _pc_version_value)
string(REGEX REPLACE "^Version:[ \t]*" "" _pc_version "${_pc_version_value}")
string(STRIP "${_pc_version}" _pc_version)

file(READ "${_version_file}" _version_config)
if(NOT _version_config MATCHES "set\\(PACKAGE_VERSION \"([^\"]+)\"\\)")
    message(FATAL_ERROR
        "LanceCConfigVersion.cmake does not declare PACKAGE_VERSION")
endif()
set(_package_version "${CMAKE_MATCH_1}")
if(NOT "${_pc_version}" STREQUAL "${_package_version}")
    message(FATAL_ERROR
        "pkg-config version ${_pc_version} does not match "
        "CMake package version ${_package_version}")
endif()

if(DEFINED PLATFORM AND NOT PLATFORM STREQUAL "")
    set(_platform_label "${PLATFORM}")
else()
    set(_platform_label "the exported dependencies")
endif()

message(STATUS
    "Installed metadata matches ${_platform_label}: "
    "version ${_package_version}, Libs.private '${_pc_private_libs}'")
