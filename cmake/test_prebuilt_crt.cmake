# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Exercise the actual Windows prebuilt configure/install metadata boundary.
# No binaries are built: the input directory is a packaging fixture, and the
# caller explicitly declares its CRT. Native consumers are tested separately.
cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_HOST_WIN32)
    message(FATAL_ERROR "Run this test with a native Windows CMake toolchain")
endif()
foreach(_required TEST_ROOT TEST_GENERATOR TEST_PLATFORM)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "Missing ${_required}")
    endif()
endforeach()

get_filename_component(_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_input "${TEST_ROOT}/input")
file(MAKE_DIRECTORY "${_input}/include" "${_input}/lib")
set(_toolset)
if(DEFINED TEST_TOOLSET AND NOT TEST_TOOLSET STREQUAL "")
    list(APPEND _toolset -T "${TEST_TOOLSET}")
endif()

foreach(_case missing ON OFF)
    set(_build "${TEST_ROOT}/build-${_case}")
    set(_install "${TEST_ROOT}/install-${_case}")
    # Each case starts without cached source-build options.
    file(REMOVE_RECURSE "${_build}" "${_install}")
    set(_crt)
    if(NOT _case STREQUAL "missing")
        list(APPEND _crt "-DLANCE_C_CRT_STATIC=${_case}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
            -G "${TEST_GENERATOR}" -A "${TEST_PLATFORM}" ${_toolset}
            -DLANCE_C_USE_PREBUILT=ON "-DLANCE_C_PREBUILT_DIR=${_input}"
            -DCMAKE_INSTALL_LIBDIR=lib ${_crt}
        RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(_case STREQUAL "missing")
        if(_status EQUAL 0 OR NOT "${_out}${_err}" MATCHES
                "Windows prebuilt CRT must be declared")
            message(FATAL_ERROR "Undeclared prebuilt CRT was not rejected: ${_out}${_err}")
        endif()
    else()
        if(NOT _status EQUAL 0)
            message(FATAL_ERROR "Prebuilt ${_case} configure failed: ${_out}${_err}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" --install "${_build}"
                --prefix "${_install}" --config Release
            RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        if(NOT _status EQUAL 0)
            message(FATAL_ERROR "Prebuilt ${_case} install failed: ${_out}${_err}")
        endif()
        file(READ "${_install}/lib/cmake/LanceC/LanceCConfig.cmake" _config)
        string(FIND "${_config}" "set(LanceC_STATIC_CRT ${_case})" _declaration)
        if(_declaration EQUAL -1)
            message(FATAL_ERROR "Installed metadata lost the explicit ${_case} CRT declaration")
        endif()
    endif()
    message(STATUS "Prebuilt CRT ${_case}: PASS")
endforeach()
