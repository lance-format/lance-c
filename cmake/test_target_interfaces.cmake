# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Configure and compile C11, C++17 and C++20 consumers without building Rust.
# Run with: cmake -P cmake/test_target_interfaces.cmake
cmake_minimum_required(VERSION 3.22)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED TEST_ROOT)
    set(TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/build/test-target-interfaces")
endif()
file(REMOVE_RECURSE "${TEST_ROOT}")
# Preserve a redundant separator like the one produced by a trailing slash in
# macOS TMPDIR. Consumers must compare equivalent paths after normalization.
set(_prefix "${TEST_ROOT}//prebuilt")
file(MAKE_DIRECTORY "${_prefix}/lib")
file(COPY "${_repo}/include/" DESTINATION "${_prefix}/include")
# Only metadata and compilation are tested; these placeholders are never linked.
foreach(_library liblance_c.a liblance_c.so liblance_c.dylib
        lance_c.lib lance_c.dll lance_c.dll.lib)
    file(WRITE "${_prefix}/lib/${_library}" "")
endforeach()

function(_run)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE _status OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "Command failed (${_status}): ${ARGV}\n${_out}\n${_err}")
    endif()
endfunction()

_run("${CMAKE_COMMAND}" -S "${_repo}" -B "${TEST_ROOT}/package"
    -DLANCE_C_USE_PREBUILT=ON "-DLANCE_C_PREBUILT_DIR=${_prefix}"
    -DLANCE_C_CRT_STATIC=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_LIBDIR=lib)
_run("${CMAKE_COMMAND}" --install "${TEST_ROOT}/package"
    --prefix "${TEST_ROOT}/installed" --config Release)

foreach(_mode installed subdirectory fetchcontent)
    foreach(_linkage static shared)
        foreach(_language C CXX17 CXX20)
            set(_build "${TEST_ROOT}/${_mode}-${_linkage}-${_language}")
            set(_wrapper OFF)
            set(_cmake_language C)
            if(_language MATCHES "CXX")
                set(_cmake_language CXX)
            endif()
            if(_language STREQUAL "CXX20")
                set(_wrapper ON)
            endif()
            _run("${CMAKE_COMMAND}"
                -S "${_repo}/cmake/tests/target-consumer" -B "${_build}"
                "-DTEST_REPO=${_repo}" "-DTEST_PREFIX=${_prefix}"
                "-DCMAKE_PREFIX_PATH=${TEST_ROOT}/installed"
                "-DTEST_MODE=${_mode}" "-DTEST_LANGUAGE=${_cmake_language}"
                "-DTEST_CPP_WRAPPER=${_wrapper}" "-DLanceC_LINK=${_linkage}"
                -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release)
            _run("${CMAKE_COMMAND}" --build "${_build}" --config Release
                --target consumer)
            message(STATUS "${_mode} / ${_linkage} / ${_language}: passed")
        endforeach()
    endforeach()
endforeach()

# An installed Corrosion package must be usable with downloads disabled. This
# fixture supplies only its import API; no Rust code is compiled or patched.
set(_corrosion "${TEST_ROOT}/corrosion")
file(MAKE_DIRECTORY "${_corrosion}")
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${_corrosion}/CorrosionConfigVersion.cmake"
    VERSION 0.5.1 COMPATIBILITY SameMinorVersion)
file(WRITE "${_corrosion}/CorrosionConfig.cmake" [=[
function(corrosion_import_crate)
    add_library(lance_c-static STATIC IMPORTED GLOBAL)
    add_library(lance_c-shared SHARED IMPORTED GLOBAL)
    set_target_properties(lance_c-static PROPERTIES
        IMPORTED_LOCATION "${TEST_PREFIX}/lib/liblance_c.a")
    set_target_properties(lance_c-shared PROPERTIES
        IMPORTED_LOCATION "${TEST_PREFIX}/lib/liblance_c.so"
        IMPORTED_IMPLIB "${TEST_PREFIX}/lib/lance_c.dll.lib")
endfunction()
function(corrosion_set_env_vars)
endfunction()
]=])
_run("${CMAKE_COMMAND}" -S "${_repo}" -B "${TEST_ROOT}/offline"
    "-DCMAKE_PREFIX_PATH=${_corrosion}" "-DTEST_PREFIX=${_prefix}"
    -DLANCE_C_USE_PREBUILT=OFF -DLANCE_C_CRT_STATIC=OFF
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DBUILD_TESTING=OFF)

message(STATUS "All target interface tests passed.")
