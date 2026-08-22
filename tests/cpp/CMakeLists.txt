# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_tests_cpp CXX C)

# Emit compile_commands.json so `ove lint` can feed clang-tidy real
# -I/-D flags for the bindings/cpp surface.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
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

# Include test helpers
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/OveTest.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/OveCoverage.cmake)

# Stub backend sources (same as C tests)
set(POSIX_CORE_DIR ${OVE_ROOT}/backends/posix)
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../backends/stub)
set(STUB_SOURCES
    ${POSIX_CORE_DIR}/posix_thread.c
    ${POSIX_CORE_DIR}/posix_sync.c
    ${OVE_SRC}/ove_sync.c
    ${POSIX_CORE_DIR}/posix_queue.c
    ${OVE_SRC}/ove_queue.c
    ${POSIX_CORE_DIR}/posix_timer.c
    ${POSIX_CORE_DIR}/posix_time.c
    ${POSIX_CORE_DIR}/posix_console.c
    ${POSIX_CORE_DIR}/posix_eventgroup.c
    ${OVE_SRC}/ove_eventgroup.c
    ${POSIX_CORE_DIR}/posix_workqueue.c
    ${POSIX_CORE_DIR}/posix_stream.c
    ${OVE_SRC}/ove_stream.c
    ${POSIX_CORE_DIR}/posix_watchdog.c
    ${POSIX_CORE_DIR}/posix_nvs.c
    ${POSIX_CORE_DIR}/posix_shell.c
    ${POSIX_CORE_DIR}/posix_fs.c
    ${OVE_SRC}/ove_fs.c
    ${OVE_SRC}/ove_media.c
    ${OVE_SRC}/ove_net.c
    ${POSIX_CORE_DIR}/posix_net.c
    # Async runtime substrate (compiled to no-ops when CONFIG_OVE_ASYNC=n).
    ${POSIX_CORE_DIR}/posix_irq.c
    # Shared BSP modules
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    # Audio graph engine
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    # Heap-lock machinery: portable wrappers + stub-only weak __real_*
    # fallbacks (analogue of {freertos,zephyr,nuttx}_heap_lock.c).
    ${OVE_ROOT}/backends/common/ove_heap_lock.c
    ${STUB_DIR}/stub_heap_lock.c
    # Headless stubs
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    # App framework
    ${OVE_SRC}/ove_app.c
)

# C++ test suite sources — single-sourced from framework/suites.inc (the
# x-macro that also drives cpp_main.cpp dispatch + ove_test.hpp runner
# declarations).  ove_test_cpp_suites() validates the .inc against the
# suites/test_*.cpp files and fails configure on any drift.
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/suites)
ove_test_cpp_suites(${CMAKE_CURRENT_SOURCE_DIR}/framework/suites.inc ${SUITE_DIR} SUITE_SOURCES)

# ove_add_cpp_variant(<name> [compile_opts...])
# Mirrors ove_add_stub_variant in tests/CMakeLists.txt: shared sources +
# per-variant compile/link flags so we can stamp out coverage/asan builds
# without copy-pasting.
function(ove_add_cpp_variant name)
    add_executable(${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/cpp_main.cpp
        ${STUB_SOURCES}
        ${SUITE_SOURCES}
    )
    target_include_directories(${name} PRIVATE
        ${OVE_INCLUDE}
        ${OVE_ROOT}/bindings/cpp
        ${OVE_ROOT}/backends/common
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${POSIX_CORE_DIR}/include
        ${cmocka_SOURCE_DIR}/include
    )
    target_link_libraries(${name} PRIVATE cmocka-static pthread rt m)
    # Enable the net wrappers (and their POSIX BSD-socket backend) so the
    # net storage-ownership regression suite can exercise move + accept.
    target_compile_definitions(${name} PRIVATE CONFIG_OVE_NET=1)
    if(ARGN)
        target_compile_options(${name} PRIVATE ${ARGN})
        target_link_options(${name} PRIVATE ${ARGN})
    endif()
endfunction()

# --- C++ test target ---
ove_add_cpp_variant(ove_test_cpp)

# --- Coverage variant — gcov instrumentation for the `coverage` target. ---
# See tests/CMakeLists.txt for why we build a second zero-heap twin — the
# *_init() static-storage paths would otherwise never be exercised.
option(OVE_TEST_BUILD_COVERAGE "Build ove_test_cpp_coverage with --coverage" OFF)
if(OVE_TEST_BUILD_COVERAGE)
    # `-fprofile-update=atomic` makes gcov counter increments atomic across
    # threads.  Required by tests like test_lockguard.cpp that exercise the
    # same source line from multiple threads (LockGuard contention loop) —
    # without it, counter races can underflow and lcov 2.1+ rejects the
    # tracefile with "Unexpected negative count".
    ove_add_cpp_variant(ove_test_cpp_coverage --coverage -fprofile-update=atomic -O0 -g)
    ove_add_cpp_variant(ove_test_cpp_coverage_zh --coverage -fprofile-update=atomic -O0 -g)
    target_compile_definitions(ove_test_cpp_coverage_zh PRIVATE CONFIG_OVE_ZERO_HEAP=1)
    ove_test_coverage_report(cpp
        ove_test_cpp_coverage
        ove_test_cpp_coverage_zh)
endif()

# Register with CTest
enable_testing()
add_test(NAME ove_cpp_tests COMMAND ove_test_cpp)
