# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Self-contained tests for read_cargo_version. Run with:
#   cmake -P cmake/test_cargo_version.cmake

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/cargo-version.cmake")

set(_tmp_dir "${CMAKE_CURRENT_LIST_DIR}/.test_cargo_version_tmp")
file(MAKE_DIRECTORY "${_tmp_dir}")

function(_check label cargo_body expected_clean expected_full)
    set(_toml "${_tmp_dir}/${label}.toml")
    file(WRITE "${_toml}" "${cargo_body}")

    read_cargo_version("${_toml}" v)

    if(NOT v STREQUAL "${expected_clean}")
        message(FATAL_ERROR "[${label}] expected v='${expected_clean}', got '${v}'")
    endif()
    if(NOT v_FULL STREQUAL "${expected_full}")
        message(FATAL_ERROR "[${label}] expected v_FULL='${expected_full}', got '${v_FULL}'")
    endif()
    message(STATUS "[${label}] OK: v=${v}, v_FULL=${v_FULL}")
endfunction()

_check(stable
    "[package]\nname = \"lance-c\"\nversion = \"0.1.0\"\n"
    "0.1.0" "0.1.0")

_check(beta
    "[package]\nname = \"lance-c\"\nversion = \"0.2.0-beta.1\"\n"
    "0.2.0" "0.2.0-beta.1")

_check(beta_double_digit
    "[package]\nname = \"lance-c\"\nversion = \"0.1.1-beta.10\"\n"
    "0.1.1" "0.1.1-beta.10")

_check(rc
    "[package]\nname = \"lance-c\"\nversion = \"1.0.0-rc.3\"\n"
    "1.0.0" "1.0.0-rc.3")

_check(major_release
    "[package]\nname = \"lance-c\"\nversion = \"2.0.0\"\n"
    "2.0.0" "2.0.0")

# Cleanup
file(REMOVE_RECURSE "${_tmp_dir}")
message(STATUS "All read_cargo_version tests passed.")
