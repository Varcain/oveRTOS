# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_test_zephyr)

# Fetch CMocka
include(FetchContent)
FetchContent_Declare(
    cmocka
    GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
    GIT_TAG        cmocka-1.1.7
)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cmocka)

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
# Console uses stub because native_sim lacks UART interrupt support
# needed by Zephyr's console_getchar()/console_init().
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

target_link_libraries(app PRIVATE cmocka-static m)

# Dispatch-overhead symbol audit on the test ELF (Phase 3 of
# moat-hardening). Zephyr's final ELF is built by an internal subscope
# this directory cannot attach POST_BUILD steps to — use ELF_PATH for
# the file-level dependency hook (same pattern as the bench target in
# boards/stm32f746g-discovery/zephyr/CMakeLists.txt). Symbol audit only;
# hotpath disasm runs only on bench targets (test ELFs are CMocka
# scaffolding, not the production hot path).
include(${OVE_ROOT}/cmake/OveZeroOverheadAudit.cmake)
ove_assert_no_dispatch_overhead(app
    BINDING c
    ELF_PATH ${PROJECT_BINARY_DIR}/zephyr/zephyr.elf)
