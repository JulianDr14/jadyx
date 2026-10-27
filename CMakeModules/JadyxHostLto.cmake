# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later

## JadyxHostLto ##

# Whole-program optimization for the emulator's own hot code on the UWP Release build.
# Unlike the global ENABLE_LTO, it only touches the targets that run every frame (CPU core, GPU
# front end, renderer, JIT and the frontend that links them), so third-party libraries keep their
# normal objects and the time zone data keeps its source path. MSVC compiles those targets with
# /GL and links jadyx with /LTCG, which lets calls cross library boundaries (common <-> core <->
# video_core) inline and drops the code nothing reaches.

option(JADYX_HOST_LTO "Link-time optimize the emulator's own libraries in Release" OFF)

function(jadyx_enable_host_lto)
    if (NOT JADYX_HOST_LTO OR ENABLE_LTO)
        # Off, or the global ENABLE_LTO already covers every target.
        return()
    endif()

    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_log LANGUAGES C CXX)
    if (NOT _ipo_ok)
        message(FATAL_ERROR "JADYX_HOST_LTO: the compiler can't link-time optimize:\n${_ipo_log}")
    endif()

    set(_lto_targets)
    foreach(_target IN LISTS ARGN)
        if (TARGET ${_target})
            get_target_property(_aliased ${_target} ALIASED_TARGET)
            if (_aliased)
                set(_target ${_aliased})
            endif()
            set_property(TARGET ${_target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
            list(APPEND _lto_targets ${_target})
        endif()
    endforeach()
    message(STATUS "JADYX_HOST_LTO: link-time optimizing ${_lto_targets}")
endfunction()
