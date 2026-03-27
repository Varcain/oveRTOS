# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_tests_cpp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Strict warnings
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)
# cmocka uses volatile-qualified parameters deprecated in C++20
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-volatile>)

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
set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)

# Include test helper
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/OveTest.cmake)

# Stub backend sources (same as C tests)
set(POSIX_CORE_DIR ${OVE_ROOT}/backends/posix)
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../backends/stub)
set(STUB_SOURCES
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
    # Audio graph engine
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    # Headless stubs
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    # App framework
    ${OVE_SRC}/ove_app.c
)

# C++ test suite sources
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/suites)
set(SUITE_SOURCES
    ${SUITE_DIR}/test_mutex.cpp
    ${SUITE_DIR}/test_recursive_mutex.cpp
    ${SUITE_DIR}/test_semaphore.cpp
    ${SUITE_DIR}/test_event.cpp
    ${SUITE_DIR}/test_condvar.cpp
    ${SUITE_DIR}/test_queue.cpp
    ${SUITE_DIR}/test_timer.cpp
    ${SUITE_DIR}/test_thread.cpp
    ${SUITE_DIR}/test_eventgroup.cpp
    ${SUITE_DIR}/test_lockguard.cpp
    ${SUITE_DIR}/test_app.cpp
    ${SUITE_DIR}/test_console.cpp
    ${SUITE_DIR}/test_time.cpp
    ${SUITE_DIR}/test_watchdog.cpp
    ${SUITE_DIR}/test_nvs.cpp
    ${SUITE_DIR}/test_shell.cpp
    ${SUITE_DIR}/test_bsp.cpp
    ${SUITE_DIR}/test_board.cpp
    ${SUITE_DIR}/test_gpio.cpp
    ${SUITE_DIR}/test_led.cpp
    ${SUITE_DIR}/test_audio.cpp
    ${SUITE_DIR}/test_fs.cpp
    ${SUITE_DIR}/test_stream.cpp
    ${SUITE_DIR}/test_workqueue.cpp
    ${SUITE_DIR}/test_static_init.cpp
)

# --- C++ test target ---
add_executable(ove_test_cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp_main.cpp
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_include_directories(ove_test_cpp PRIVATE
    ${OVE_INCLUDE}
    ${OVE_ROOT}/bindings/cpp
    ${OVE_ROOT}/backends/common
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${POSIX_CORE_DIR}/include
    ${cmocka_SOURCE_DIR}/include
)

target_link_libraries(ove_test_cpp PRIVATE
    cmocka-static
    pthread
    rt
    m
)

# Register with CTest
enable_testing()
add_test(NAME ove_cpp_tests COMMAND ove_test_cpp)
