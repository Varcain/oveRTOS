# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Report firmware memory usage per physical region.
#
# Usage: cmake -DELF_FILE=<path> -DSIZE_TOOL=<tool> -DMAP_FILE=<path>
#              -P print_size.cmake
#
# `size --format=berkeley` sums every BSS section regardless of address, so on a
# board with external SDRAM it charges the multi-megabyte guest pools and the
# framebuffer against the internal RAM total and reports >2000% — true as
# arithmetic, useless as a number to act on.  Bucket sections by their VMA into
# the regions the linker actually declared instead.
#
# Regions are taken from the .map's own Memory Configuration table, where the
# linker has already evaluated expressions like `8M - 0x800`, so this neither
# duplicates nor re-implements the .ld.

foreach(_req ELF_FILE SIZE_TOOL MAP_FILE)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR "print_size: ${_req} is required")
    endif()
endforeach()
foreach(_f ELF_FILE MAP_FILE)
    if(NOT EXISTS "${${_f}}")
        message(FATAL_ERROR "print_size: ${_f} not found: ${${_f}}")
    endif()
endforeach()

function(_pad_to _text _width _out)
    set(_s "${_text}")
    string(LENGTH "${_s}" _l)
    while(_l LESS ${_width})
        string(APPEND _s " ")
        math(EXPR _l "${_l} + 1")
    endwhile()
    set(${_out} "${_s}" PARENT_SCOPE)
endfunction()

# ── regions: the linker's own evaluated table ────────────────────────────────
file(READ "${MAP_FILE}" _map)
string(FIND "${_map}" "Memory Configuration" _a)
string(FIND "${_map}" "Linker script and memory map" _b)
if(_a EQUAL -1 OR _b EQUAL -1 OR NOT _b GREATER _a)
    message(FATAL_ERROR "print_size: no Memory Configuration table in ${MAP_FILE}")
endif()
math(EXPR _mc_len "${_b} - ${_a}")
string(SUBSTRING "${_map}" ${_a} ${_mc_len} _mc)
unset(_map)
string(REPLACE ";" "\\;" _mc "${_mc}")
string(REPLACE "\n" ";" _mc_lines "${_mc}")

set(REGIONS "")
foreach(_line IN LISTS _mc_lines)
    if(_line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)[ \t]+0x([0-9a-fA-F]+)[ \t]+0x([0-9a-fA-F]+)")
        set(_n "${CMAKE_MATCH_1}")
        math(EXPR _o "0x${CMAKE_MATCH_2}")
        math(EXPR _l "0x${CMAKE_MATCH_3}")
        # *default* spans the whole address space and would swallow everything.
        if(_l GREATER 0)
            list(APPEND REGIONS "${_n}")
            set(RGN_${_n}_ORIGIN ${_o})
            set(RGN_${_n}_LENGTH ${_l})
            set(RGN_${_n}_USED 0)
            set(RGN_${_n}_TOP ${_o})
        endif()
    endif()
endforeach()
if(NOT REGIONS)
    message(FATAL_ERROR "print_size: parsed no regions from ${MAP_FILE}")
endif()

# ── sections, bucketed by VMA ────────────────────────────────────────────────
execute_process(COMMAND ${SIZE_TOOL} -A -x ${ELF_FILE}
                OUTPUT_VARIABLE _size_out RESULT_VARIABLE _rc
                OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "print_size: ${SIZE_TOOL} -A failed (${_rc})")
endif()
string(REPLACE ";" "\\;" _size_out "${_size_out}")
string(REPLACE "\n" ";" _size_lines "${_size_out}")

set(_unplaced "")
foreach(_line IN LISTS _size_lines)
    if(_line MATCHES "^(\\.[^ \t]+)[ \t]+0x([0-9a-fA-F]+)[ \t]+0x([0-9a-fA-F]+)")
        set(_sn "${CMAKE_MATCH_1}")
        math(EXPR _ss "0x${CMAKE_MATCH_2}")
        math(EXPR _sa "0x${CMAKE_MATCH_3}")
        # addr 0 = not allocated: debug info, .comment, .ARM.attributes.
        if(_ss GREATER 0 AND _sa GREATER 0)
            set(_hit "")
            foreach(_r IN LISTS REGIONS)
                math(EXPR _r_end "${RGN_${_r}_ORIGIN} + ${RGN_${_r}_LENGTH}")
                if(_sa GREATER_EQUAL ${RGN_${_r}_ORIGIN} AND _sa LESS ${_r_end})
                    math(EXPR RGN_${_r}_USED "${RGN_${_r}_USED} + ${_ss}")
                    math(EXPR _sec_end "${_sa} + ${_ss}")
                    if(_sec_end GREATER ${RGN_${_r}_TOP})
                        set(RGN_${_r}_TOP ${_sec_end})
                    endif()
                    set(_hit "${_r}")
                    break()
                endif()
            endforeach()
            if(NOT _hit)
                list(APPEND _unplaced "${_sn}")
            endif()
        endif()
    endif()
endforeach()

# ── report ───────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "Memory usage by physical region")
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
_pad_to("region" 12 _h1)
_pad_to("origin" 12 _h2)
_pad_to("used" 10 _h3)
_pad_to("size" 10 _h4)
_pad_to("headroom" 10 _h5)
message(STATUS "${_h1}${_h2}${_h3}${_h4}${_h5}use")

foreach(_r IN LISTS REGIONS)
    if(RGN_${_r}_USED EQUAL 0)
        continue()
    endif()
    set(_used ${RGN_${_r}_USED})
    set(_len ${RGN_${_r}_LENGTH})
    math(EXPR _origin "${RGN_${_r}_ORIGIN}" OUTPUT_FORMAT HEXADECIMAL)
    # Headroom is measured from the END of the last section, not size-minus-sum.
    # Alignment holes between sections cannot be reclaimed by a section growing
    # into them, so subtracting the sum overstates what is really addable.
    math(EXPR _head "${RGN_${_r}_ORIGIN} + ${_len} - ${RGN_${_r}_TOP}")
    math(EXPR _pct "${_used} * 100 / ${_len}")

    _pad_to("${_r}" 12 _c1)
    _pad_to("${_origin}" 12 _c2)
    _pad_to("${_used}" 10 _c3)
    _pad_to("${_len}" 10 _c4)
    _pad_to("${_head}" 10 _c5)
    message(STATUS "${_c1}${_c2}${_c3}${_c4}${_c5}${_pct}%")
endforeach()

if(_unplaced)
    list(REMOVE_DUPLICATES _unplaced)
    string(REPLACE ";" " " _unplaced_s "${_unplaced}")
    message(STATUS "")
    message(STATUS "  allocated outside every declared region: ${_unplaced_s}")
endif()
message(STATUS "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
message(STATUS "")
