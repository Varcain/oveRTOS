# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# oveRTOS Zephyr board CMakeLists.txt for STM32F746G-Discovery.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZephyrCommon.cmake)

ove_zephyr_pre_find()

# Add out-of-tree WM8994 driver
list(APPEND ZEPHYR_EXTRA_MODULES ${OVE_BOARD_DIR}/drivers/wm8994)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

target_sources(app PRIVATE
    ${OVE_BOARD_DIR}/main.c

    # oveRTOS framework + backend (only enabled modules, from ove_config.cmake)
    ${OVE_BACKEND_SOURCES}
)

ove_zephyr_add_tflm_if_enabled()
ove_zephyr_include_app_sources()
ove_zephyr_add_common_includes()

# Zero-heap link-time audit: fail the link if a kernel/libc heap
# region is instantiated.  STRICT also forbids __HeapBase / __HeapLimit
# since production zero-heap builds use picolibc with no libc heap.
#
# No --wrap on production Zephyr.  Picolibc's nano-malloc and Zephyr's
# libc-common malloc both ship `malloc` symbols and are both pulled in
# under --whole-archive when --wrap is active, producing a multiple-
# definition error.  Zephyr+zeroheap has no kernel heap by design
# (audited below), so the wrap chain has nothing to intercept anyway.
if(OVE_ZERO_HEAP)
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZeroHeapAudit.cmake)
    ove_zero_heap_assert_no_kernel_alloc(zephyr_final
        STRICT
        ELF_PATH ${PROJECT_BINARY_DIR}/zephyr/zephyr.elf)
endif()

#############################################################################
# Benchmark apps (live under tests/benchmarks/<lang>/): force -O2 +
# -fno-omit-frame-pointer.  Same audit/hotpath pipeline as FreeRTOS /
# NuttX, attached to Zephyr's `zephyr_final` target with cross-scope
# ELF_PATH (Zephyr's final-link is in a Zephyr-internal subscope).
#############################################################################
if(OVE_APP_DIR MATCHES "/tests/benchmarks/")
    target_compile_options(app PRIVATE -O2 -fno-omit-frame-pointer)
    target_compile_definitions(app PRIVATE
        OVE_APP_LANG_NAME=\"${OVE_APP_LANG}\")
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZeroOverheadAudit.cmake)
    set(_audit_target_key "stm32f746_zephyr_benchmark_${OVE_APP_LANG}")
    ove_assert_no_dispatch_overhead(app
        BINDING ${OVE_APP_LANG}
        ELF_PATH ${PROJECT_BINARY_DIR}/zephyr/zephyr.elf)
    ove_dump_hotpaths(app
        BINDING ${OVE_APP_LANG}
        TARGET_KEY ${_audit_target_key}
        ELF_PATH ${PROJECT_BINARY_DIR}/zephyr/zephyr.elf)
endif()
