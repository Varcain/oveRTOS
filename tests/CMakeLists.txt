# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)

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

# oveRTOS source paths
set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)

# Include test helper
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OveTest.cmake)

# Stub backend sources (direct-linked, no dispatcher layer)
set(POSIX_CORE_DIR ${OVE_ROOT}/backends/posix)
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends/stub)
set(STUB_SOURCES
    # Core POSIX (shared with real backend)
    ${POSIX_CORE_DIR}/posix_thread.c
    ${POSIX_CORE_DIR}/posix_sync.c
    ${POSIX_CORE_DIR}/posix_queue.c
    ${POSIX_CORE_DIR}/posix_timer.c
    ${POSIX_CORE_DIR}/posix_time.c
    ${POSIX_CORE_DIR}/posix_console.c
    ${POSIX_CORE_DIR}/posix_eventgroup.c
    ${POSIX_CORE_DIR}/posix_workqueue.c
    ${POSIX_CORE_DIR}/posix_stream.c
    ${POSIX_CORE_DIR}/posix_watchdog.c
    ${POSIX_CORE_DIR}/posix_nvs.c
    ${POSIX_CORE_DIR}/posix_shell.c
    ${POSIX_CORE_DIR}/posix_fs.c
    # Shared BSP modules
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    # Headless stubs (display, audio, HAL)
    ${STUB_DIR}/stub_audio.c
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    # App framework
    ${OVE_SRC}/ove_app.c
)

# Test suite sources
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/suites)
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
    ${SUITE_DIR}/test_static_define.c
    ${SUITE_DIR}/test_app.c
)

# --- Stub test target ---
add_executable(ove_test_stub
    ${CMAKE_CURRENT_SOURCE_DIR}/stub_main.c
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_include_directories(ove_test_stub PRIVATE
    ${OVE_INCLUDE}
    ${OVE_ROOT}/backends/common
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${POSIX_CORE_DIR}/include
    ${cmocka_SOURCE_DIR}/include
)

target_link_libraries(ove_test_stub PRIVATE
    cmocka-static
    pthread
    rt
)

# Register with CTest
enable_testing()
add_test(NAME ove_stub_tests COMMAND ove_test_stub)
