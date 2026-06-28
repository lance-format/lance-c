# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors

include_guard(GLOBAL)

function(lance_c_load_baseline_env env_file)
    if(NOT EXISTS "${env_file}")
        message(FATAL_ERROR "Missing baseline env file: ${env_file}")
    endif()

    file(STRINGS "${env_file}" _baseline_lines)
    foreach(_line IN LISTS _baseline_lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endforeach()
endfunction()
