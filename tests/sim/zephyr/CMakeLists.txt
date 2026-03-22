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

# --- Pure Zephyr backend modules (10, direct-linked) ---
set(ZEPHYR_BACKEND_SOURCES
    ${ZEPHYR_BACKEND}/zephyr_thread.c
    ${ZEPHYR_BACKEND}/zephyr_sync.c
    ${ZEPHYR_BACKEND}/zephyr_queue.c
    ${ZEPHYR_BACKEND}/zephyr_timer.c
    ${ZEPHYR_BACKEND}/zephyr_time.c
    ${ZEPHYR_BACKEND}/zephyr_eventgroup.c
    ${ZEPHYR_BACKEND}/zephyr_workqueue.c
    ${ZEPHYR_BACKEND}/zephyr_stream.c
    ${ZEPHYR_BACKEND}/zephyr_shell.c
)

# --- Core modules (board/GPIO/LED dispatcher) ---
set(OVE_SRC ${OVE_ROOT}/src)
set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
)

# --- Stub backend for hardware-dependent modules (7) ---
# Console uses stub because native_sim lacks UART interrupt support
# needed by Zephyr's console_getchar()/console_init().
set(STUB_SOURCES
    ${STUB_DIR}/stub_console.c
    ${STUB_DIR}/stub_watchdog.c
    ${STUB_DIR}/stub_nvs.c
    ${STUB_DIR}/stub_fs.c
    ${STUB_DIR}/stub_audio.c
    ${STUB_DIR}/stub_bsp.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_lvgl.c
)

# --- Test suites (20) ---
set(SUITE_SOURCES
    ${SUITE_DIR}/test_thread.c
    ${SUITE_DIR}/test_sync_mutex.c
    ${SUITE_DIR}/test_sync_sem.c
    ${SUITE_DIR}/test_sync_event.c
    ${SUITE_DIR}/test_sync_condvar.c
    ${SUITE_DIR}/test_sync_recursive.c
    ${SUITE_DIR}/test_queue.c
    ${SUITE_DIR}/test_timer.c
    ${SUITE_DIR}/test_time.c
    ${SUITE_DIR}/test_eventgroup.c
    ${SUITE_DIR}/test_workqueue.c
    ${SUITE_DIR}/test_stream.c
    ${SUITE_DIR}/test_console.c
    ${SUITE_DIR}/test_watchdog.c
    ${SUITE_DIR}/test_nvs.c
    ${SUITE_DIR}/test_shell.c
    ${SUITE_DIR}/test_audio.c
    ${SUITE_DIR}/test_bsp.c
    ${SUITE_DIR}/test_board.c
    ${SUITE_DIR}/test_gpio.c
    ${SUITE_DIR}/test_led.c
    ${SUITE_DIR}/test_lvgl.c
    ${SUITE_DIR}/test_app.c
)

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

target_link_libraries(app PRIVATE cmocka-static)
