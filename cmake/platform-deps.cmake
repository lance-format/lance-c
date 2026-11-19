# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
#
# Platform link dependencies for the build tree, the installed CMake package,
# and pkg-config metadata. All three views are generated from this list so a
# platform change cannot leave the installed metadata out of sync.

include_guard(GLOBAL)

# Set <out_var> to the CMake link items required on <system_name> (the value of
# CMAKE_SYSTEM_NAME, such as Linux, Darwin, or Windows).
function(lance_c_platform_dependencies out_var system_name)
    if(system_name STREQUAL "Darwin")
        set(_deps
            "-framework CoreFoundation"
            "-framework IOKit"
            "-framework Security"
            "-framework SystemConfiguration")
    elseif(system_name STREQUAL "Linux")
        set(_deps pthread dl m)
    elseif(system_name STREQUAL "Windows")
        set(_deps advapi32 bcrypt dbghelp kernel32 mswsock ntdll ole32 userenv ws2_32)
    else()
        set(_deps "")
    endif()
    set(${out_var} "${_deps}" PARENT_SCOPE)
endfunction()

# Format a CMake dependency list for pkg-config's Libs.private. Framework
# entries keep their two-word linker form; ordinary libraries become -l<name>.
function(lance_c_deps_to_pkg_config out_var deps)
    set(_libs "")
    foreach(_dep IN LISTS deps)
        if(_dep MATCHES "^-framework (.+)$")
            list(APPEND _libs "-framework" "${CMAKE_MATCH_1}")
        else()
            list(APPEND _libs "-l${_dep}")
        endif()
    endforeach()
    if(_libs)
        string(JOIN " " _libs_text ${_libs})
    else()
        set(_libs_text "")
    endif()
    set(${out_var} "${_libs_text}" PARENT_SCOPE)
endfunction()

# Set <out_var> to the pkg-config Libs.private value for <system_name>.
function(lance_c_pkg_config_private_libs out_var system_name)
    lance_c_platform_dependencies(_deps "${system_name}")
    lance_c_deps_to_pkg_config(_libs_text "${_deps}")
    set(${out_var} "${_libs_text}" PARENT_SCOPE)
endfunction()
