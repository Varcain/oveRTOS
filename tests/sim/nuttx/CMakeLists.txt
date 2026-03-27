# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_test_nuttx C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)

include(FetchContent)

# --- Fetch CMocka ---
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
set(NUTTX_BACKEND ${OVE_ROOT}/backends/nuttx)
set(TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STUB_DIR ${TESTS_ROOT}/backends/stub)
set(SUITE_DIR ${TESTS_ROOT}/suites)

# --- NuttX backend sources (10, pure POSIX, direct-linked) ---
set(NUTTX_BACKEND_SOURCES
    ${NUTTX_BACKEND}/nuttx_thread.c
    ${NUTTX_BACKEND}/nuttx_sync.c
    ${NUTTX_BACKEND}/nuttx_queue.c
    ${NUTTX_BACKEND}/nuttx_timer.c
    ${NUTTX_BACKEND}/nuttx_time.c
    ${NUTTX_BACKEND}/nuttx_eventgroup.c
    ${NUTTX_BACKEND}/nuttx_workqueue.c
    ${NUTTX_BACKEND}/nuttx_stream.c
    ${NUTTX_BACKEND}/nuttx_console.c
    ${NUTTX_BACKEND}/nuttx_shell.c
)

# --- Core modules (board/GPIO/LED dispatcher) ---
set(OVE_SRC ${OVE_ROOT}/src)
set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
)

# --- Stub backend for hardware-dependent modules (6) ---
set(STUB_SOURCES
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
    ${SUITE_DIR}/test_fs.c
    ${SUITE_DIR}/test_lvgl.c
    ${SUITE_DIR}/test_app.c
)

# --- Executable ---
add_executable(ove_test_nuttx
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_main.c
    ${NUTTX_BACKEND_SOURCES}
    ${CORE_SOURCES}
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_include_directories(ove_test_nuttx PRIVATE
    # ove_config.h lives here (must be before TESTS_ROOT)
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVE_INCLUDE}
    ${NUTTX_BACKEND}/include
    ${OVE_ROOT}/backends/common
    ${TESTS_ROOT}
    ${cmocka_SOURCE_DIR}/include
    ${NUTTX_INCLUDE_DIR}
)

target_link_libraries(ove_test_nuttx PRIVATE
    cmocka-static
    pthread
    rt
    m
)

enable_testing()
add_test(NAME ove_nuttx_tests COMMAND ove_test_nuttx)
