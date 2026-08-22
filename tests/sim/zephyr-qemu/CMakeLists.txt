# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_test_zephyr_qemu)

# Fetch CMocka (source only — Zephyr ARM libc lacks sigjmp_buf for standard build)
include(FetchContent)
FetchContent_Declare(
    cmocka
    GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
    GIT_TAG        cmocka-1.1.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(cmocka)
if(NOT cmocka_POPULATED)
    FetchContent_Populate(cmocka)
endif()
zephyr_library_named(cmocka_lib)
zephyr_library_sources(${cmocka_SOURCE_DIR}/src/cmocka.c)
zephyr_library_include_directories(${cmocka_SOURCE_DIR}/include ${cmocka_SOURCE_DIR}/src)
zephyr_library_compile_definitions(
    HAVE_STRINGS_H HAVE_INTTYPES_H HAVE_MALLOC_H
    HAVE_SIGNAL_H
    HAVE_SETJMP_H HAVE_STDARG_H HAVE_STDDEF_H HAVE_STDINT_H
    HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
    HAVE_CALLOC HAVE_EXIT HAVE_FREE HAVE_LONGJMP HAVE_MALLOC
    HAVE_MEMCPY HAVE_MEMSET HAVE_SETJMP HAVE_STRCMP HAVE_SPRINTF
    HAVE_VSNPRINTF HAVE_FPRINTF HAVE_PRINTF
    HAVE_UINTPTR_T
)
zephyr_library_compile_options(-Wno-unused-parameter -Wno-implicit-function-declaration -Wno-int-conversion)

# --- Paths ---
set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(ZEPHYR_BACKEND ${OVE_ROOT}/backends/zephyr)
set(TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STUB_DIR ${TESTS_ROOT}/backends/stub)
set(SUITE_DIR ${TESTS_ROOT}/suites)
include(${TESTS_ROOT}/cmake/OveTest.cmake)

# --- Pure Zephyr backend modules (10, direct-linked) ---
set(ZEPHYR_BACKEND_SOURCES
    ${ZEPHYR_BACKEND}/zephyr_thread.c
    ${ZEPHYR_BACKEND}/zephyr_sync.c
    ${OVE_ROOT}/src/ove_sync.c
    ${ZEPHYR_BACKEND}/zephyr_queue.c
    ${ZEPHYR_BACKEND}/zephyr_timer.c
    ${ZEPHYR_BACKEND}/zephyr_time.c
    ${ZEPHYR_BACKEND}/zephyr_eventgroup.c
    ${OVE_ROOT}/src/ove_eventgroup.c
    ${ZEPHYR_BACKEND}/zephyr_workqueue.c
    ${ZEPHYR_BACKEND}/zephyr_stream.c
    ${ZEPHYR_BACKEND}/zephyr_shell.c
    # Async runtime substrate (CONFIG_OVE_ASYNC).
    ${ZEPHYR_BACKEND}/zephyr_irq.c
)

# --- Core modules (board/GPIO/LED dispatcher) ---
set(OVE_SRC ${OVE_ROOT}/src)
set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
    ${OVE_SRC}/ove_media.c
)

# --- Stub backend for hardware-dependent modules (7) ---
set(STUB_SOURCES
    ${STUB_DIR}/stub_console.c
    ${STUB_DIR}/stub_watchdog.c
    ${STUB_DIR}/stub_nvs.c
    ${STUB_DIR}/stub_fs.c
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    ${STUB_DIR}/stub_bsp.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_lvgl.c
)

# --- Test suites (list sourced from cmake/OveTest.cmake) ---
ove_test_common_suite_sources(SUITE_SOURCES ${SUITE_DIR})

target_sources(app PRIVATE
    src/main.c
    ${ZEPHYR_BACKEND_SOURCES}
    ${CORE_SOURCES}
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_include_directories(app PRIVATE
    # ove_config.h lives here (must be before TESTS_ROOT)
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVE_INCLUDE}
    ${ZEPHYR_BACKEND}/include
    ${OVE_ROOT}/backends/common
    ${TESTS_ROOT}
    ${cmocka_SOURCE_DIR}/include
)

target_compile_definitions(app PRIVATE OVE_QEMU_ARM HAVE_UINTPTR_T)
zephyr_link_libraries(cmocka_lib m)

# Dispatch-overhead symbol audit (Phase 3 of moat-hardening). Zephyr
# subscope link → ELF_PATH. Symbol audit only on test scaffolding;
# hotpath disasm reserved for bench targets.
include(${OVE_ROOT}/cmake/OveZeroOverheadAudit.cmake)
ove_assert_no_dispatch_overhead(app
    BINDING c
    ELF_PATH ${PROJECT_BINARY_DIR}/zephyr/zephyr.elf)
