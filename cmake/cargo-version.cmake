# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

# Read the version field from Cargo.toml's [package] section.
#
# Sets ${out_var} to the numeric X.Y.Z portion (CMake's project(VERSION ...)
# rejects pre-release suffixes like "-beta.1"). Also sets ${out_var}_FULL to
# the complete version string including any suffix (for display/metadata).
function(read_cargo_version cargo_toml out_var)
    file(READ "${cargo_toml}" _cargo_contents)
    if(NOT _cargo_contents MATCHES "\\[package\\][^[]*\nversion[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)([^\"]*)\"")
        message(FATAL_ERROR "Could not parse version from ${cargo_toml}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(${out_var}_FULL "${CMAKE_MATCH_1}${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()
