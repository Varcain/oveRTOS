# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# OveZeroHeapAudit.cmake — link-time symbol audit for zero-heap builds.
#
# Adds a POST_BUILD step that runs `<cross>nm` on the linked ELF and
# fails the build if a kernel/libc heap *region* is instantiated.  The
# audit deliberately checks data symbols (D/B/R nm flags), not code
# symbols, because RTOS libraries pull `sys_heap_alloc` and friends
# into .text as dead code even when no heap pool is configured — only
# the data symbol of an actual heap region is a reliable signal.
#
# Forbidden data symbols are RTOS-specific.  Pick the right set with
# the RTOS argument; default is ZEPHYR.
#
# ZEPHYR:
#   default forbids:  _system_heap, z_malloc_heap  (kernel heap pools)
#   STRICT also forbids:  __HeapBase, __HeapLimit  (newlib/picolibc heap)
#
# FREERTOS:
#   default forbids:  xHeap, ucHeap, pucAlignedHeap  (heap_*.c globals;
#                     should be absent when configSUPPORT_DYNAMIC_ALLOCATION=0)
#   STRICT also forbids:  __malloc_av_, __HeapBase, __HeapLimit
#                     (newlib heap internals)
#
# NUTTX:
#   default forbids:  g_kmmheap  (CONFIG_MM_KERNEL_HEAP split-mode pool)
#   STRICT also forbids:  __HeapBase, __HeapLimit  (libc heap, NuttX
#                     normally uses g_mmheap which IS irreducible —
#                     don't include it here, see comment in the help)
#
# NuttX note: g_mmheap (the primary kernel mm region) is the
# irreducible kernel-heap carve-out for NuttX zero-heap mode.  All
# task_create / pthread / mq_open allocations during boot land there;
# post-init traffic is trapped by ove_heap_lock.  The audit catches
# REGRESSIONS that introduce additional pools, not the documented one.
#
# STRICT is for production builds that ship picolibc / no libc heap.
# Test fixtures pull newlib in for cmocka and legitimately have a
# __HeapBase region, so leave STRICT off there.
#
# Usage:
#
#   include(${OVE_DIR}/cmake/OveZeroHeapAudit.cmake)
#   ove_zero_heap_assert_no_kernel_alloc(<target> [ELF_PATH <path>])
#
# If ELF_PATH is omitted the macro uses $<TARGET_FILE:<target>> — works
# for plain CMake executables.  Pass an explicit path for Zephyr where
# the final ELF is `zephyr/zephyr.elf` (built in a CMake subscope the
# user's CMakeLists.txt cannot attach POST_BUILD steps to); the macro
# then uses a file-level dependency via `add_custom_command(OUTPUT)` +
# `add_custom_target(ALL)` to hook into the build graph from any scope.

# Pick the right `nm` for the cross compiler.  CMake exposes CMAKE_NM
# whenever a toolchain file sets it; falls back to ${CMAKE_C_COMPILER}
# minus -gcc plus -nm if the toolchain didn't.
function(_ove_pick_nm out_var)
    if(CMAKE_NM)
        set(${out_var} "${CMAKE_NM}" PARENT_SCOPE)
        return()
    endif()
    if(CMAKE_C_COMPILER MATCHES "^(.+)-gcc$")
        set(${out_var} "${CMAKE_MATCH_1}-nm" PARENT_SCOPE)
        return()
    endif()
    # Last-ditch fallback (works for native host builds, not for
    # cross-compiled targets — those should have CMAKE_NM set).
    set(${out_var} "nm" PARENT_SCOPE)
endfunction()

# ─── ove_apply_zero_heap_wrap(target) ──────────────────────────────
#
# Apply `-Wl,--wrap=malloc/calloc/realloc/free/zalloc/memalign` (and
# the NuttX kmm_* family) to a final-exe target.  Calls to libc's
# malloc are routed through __wrap_malloc in
# backends/common/ove_heap_lock.c, which checks the lock flag and
# either traps or forwards.
#
# Use on the FINAL exe target on each RTOS:
#   - FreeRTOS:  ${_OVE_PROJ_NAME}.elf
#   - Zephyr:    add the link options via add_link_options() at the
#                point Zephyr's final link picks them up
#   - NuttX:     `nuttx`
#
# Optional EXTRA <symbols> argument adds RTOS-specific wraps (e.g.
# `kmm_malloc kmm_free` for NuttX).
function(ove_apply_zero_heap_wrap target)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs EXTRA)
    cmake_parse_arguments(_OVE_W "${options}" "${oneValueArgs}"
                          "${multiValueArgs}" ${ARGN})

    set(_libc_syms malloc calloc realloc free zalloc memalign)
    set(_all_syms ${_libc_syms} ${_OVE_W_EXTRA})

    set(_link_opts)
    foreach(_sym ${_all_syms})
        list(APPEND _link_opts "LINKER:--wrap=${_sym}")
    endforeach()

    target_link_options(${target} PRIVATE ${_link_opts})
endfunction()

function(ove_zero_heap_assert_no_kernel_alloc target)
    set(options STRICT)
    set(oneValueArgs ELF_PATH RTOS)
    set(multiValueArgs)
    cmake_parse_arguments(_OVE_ZH "${options}" "${oneValueArgs}"
                          "${multiValueArgs}" ${ARGN})

    _ove_pick_nm(_nm)

    # RTOS-specific forbidden defined data symbols.  D/B/R flags are
    # real instantiated objects; T (code) symbols can be dead-code-
    # linked and don't indicate an actual heap.
    if(NOT _OVE_ZH_RTOS OR _OVE_ZH_RTOS STREQUAL "ZEPHYR")
        set(_forbidden "_system_heap|z_malloc_heap")
        if(_OVE_ZH_STRICT)
            set(_forbidden "${_forbidden}|__HeapBase|__HeapLimit")
        endif()
    elseif(_OVE_ZH_RTOS STREQUAL "FREERTOS")
        set(_forbidden "xHeap|ucHeap|pucAlignedHeap")
        if(_OVE_ZH_STRICT)
            set(_forbidden "${_forbidden}|__malloc_av_|__HeapBase|__HeapLimit")
        endif()
    elseif(_OVE_ZH_RTOS STREQUAL "NUTTX")
        # g_mmheap is the irreducible NuttX kernel-mm region — accepted.
        # g_kmmheap only exists in CONFIG_MM_KERNEL_HEAP split-mode
        # builds; presence indicates a regression that pulled split
        # mode in.
        set(_forbidden "g_kmmheap")
        if(_OVE_ZH_STRICT)
            set(_forbidden "${_forbidden}|__HeapBase|__HeapLimit")
        endif()
    else()
        message(FATAL_ERROR
            "ove_zero_heap_assert_no_kernel_alloc: unknown RTOS '${_OVE_ZH_RTOS}' "
            "(expected ZEPHYR, FREERTOS, or NUTTX)")
    endif()

    set(_stamp_dir "${CMAKE_CURRENT_BINARY_DIR}/zero_heap_audit")
    set(_stamp "${_stamp_dir}/${target}.stamp")
    file(MAKE_DIRECTORY "${_stamp_dir}")

    if(_OVE_ZH_ELF_PATH)
        # File-level dependency works across CMake subscope boundaries
        # (Zephyr's zephyr_final lives in zephyr-workspace/zephyr CMake
        # scope — `add_custom_command(TARGET ...)` would error there).
        set(_elf "${_OVE_ZH_ELF_PATH}")
        add_custom_command(OUTPUT "${_stamp}"
            DEPENDS "${_elf}"
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[zero-heap audit] checking ${_elf} for kernel allocator symbols"
            COMMAND sh -c
                    "out=\"$(${_nm} '${_elf}' | awk '$2 ~ /^[DBR]$/ && $3 ~ /^(${_forbidden})$/ {print}')\"; \
                     if [ -n \"$out\" ]; then \
                       echo '[zero-heap audit] FAIL: kernel allocator symbols are defined:'; \
                       echo \"$out\"; \
                       exit 1; \
                     fi; \
                     echo '[zero-heap audit] OK: no kernel allocator symbols defined'"
            COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
            VERBATIM
            COMMENT "Auditing ${_elf} for forbidden zero-heap kernel allocator symbols"
        )
        add_custom_target(${target}_zero_heap_audit ALL DEPENDS "${_stamp}")
    else()
        # Plain CMake executable — POST_BUILD on the target is fine.
        set(_elf "$<TARGET_FILE:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                    "[zero-heap audit] checking ${_elf} for kernel allocator symbols"
            COMMAND sh -c
                    "out=\"$(${_nm} '${_elf}' | awk '$2 ~ /^[DBR]$/ && $3 ~ /^(${_forbidden})$/ {print}')\"; \
                     if [ -n \"$out\" ]; then \
                       echo '[zero-heap audit] FAIL: kernel allocator symbols are defined:'; \
                       echo \"$out\"; \
                       exit 1; \
                     fi; \
                     echo '[zero-heap audit] OK: no kernel allocator symbols defined'"
            VERBATIM
            COMMENT "Auditing ${target} for forbidden zero-heap kernel allocator symbols"
        )
    endif()
endfunction()
