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

# Include test helpers
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OveTest.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/OveCoverage.cmake)

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
    ${POSIX_CORE_DIR}/posix_net.c
    # Shared BSP modules
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    # Audio graph engine (platform-independent)
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    # Power management (platform-independent)
    ${OVE_SRC}/ove_pm.c
    # Headless stubs (display, HAL)
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    ${STUB_DIR}/stub_pm.c
    # App framework
    ${OVE_SRC}/ove_app.c
    # Networking helpers (SHA-1, Base64 for WebSocket/HTTPD tests)
    ${OVE_ROOT}/backends/common/ove_sha1.c
    ${OVE_ROOT}/backends/common/ove_base64.c
    # MQTT topic matcher (shared between production MQTT client and unit tests)
    ${OVE_ROOT}/backends/common/ove_mqtt_topic.c
)

# Test suite sources — list defined once in cmake/OveTest.cmake.
set(SUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/suites)
ove_test_all_suite_sources(SUITE_SOURCES ${SUITE_DIR})

# ove_add_stub_variant(<name> [compile_opts...])
# Declares a stub-test executable that shares STUB_SOURCES + SUITE_SOURCES
# but applies extra compile/link options (sanitizer, coverage, ...).
function(ove_add_stub_variant name)
    add_executable(${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/stub_main.c
        ${STUB_SOURCES}
        ${SUITE_SOURCES}
    )
    target_include_directories(${name} PRIVATE
        ${OVE_INCLUDE}
        ${OVE_ROOT}/backends/common
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${POSIX_CORE_DIR}/include
        ${cmocka_SOURCE_DIR}/include
    )
    target_link_libraries(${name} PRIVATE cmocka-static pthread rt m)
    # Enable the socket API for the stub test target only. See tests/ove_config.h
    # for why this is per-target rather than global.
    target_compile_definitions(${name} PRIVATE CONFIG_OVE_NET=1)
    if(ARGN)
        target_compile_options(${name} PRIVATE ${ARGN})
        target_link_options(${name} PRIVATE ${ARGN})
    endif()
endfunction()

# --- Stub test target ---
ove_add_stub_variant(ove_test_stub)

# --- ASAN/UBSAN variant — catches UAFs, races-that-are-leaks, stack overflow,
#     UB like integer overflow/signed conversion surprises. Enable via:
#       cmake --build . --target ove_test_stub_asan && ./ove_test_stub_asan
option(OVE_TEST_BUILD_ASAN "Build ove_test_stub_asan with -fsanitize=address,undefined" ON)
if(OVE_TEST_BUILD_ASAN)
    ove_add_stub_variant(ove_test_stub_asan
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
        -g
    )
endif()

# --- Coverage variant — gcov instrumentation for the `coverage` target. ---
# Two binaries share the same gcov tree: the default (heap-allocated *_create
# path) and a CONFIG_OVE_ZERO_HEAP twin that routes ove_test_*_create() through
# the static-storage *_init() APIs in tests/framework/ove_test.h. Without the
# zero-heap twin, half the sync/queue/timer/workqueue/stream/eventgroup init
# surface stays untouched in the report.
option(OVE_TEST_BUILD_COVERAGE "Build ove_test_stub_coverage with --coverage" OFF)
if(OVE_TEST_BUILD_COVERAGE)
    ove_add_stub_variant(ove_test_stub_coverage --coverage -O0 -g)
    ove_add_stub_variant(ove_test_stub_coverage_zh --coverage -O0 -g)
    target_compile_definitions(ove_test_stub_coverage_zh PRIVATE CONFIG_OVE_ZERO_HEAP=1)
endif()

# --- CTest registration ---
enable_testing()
add_test(NAME ove_stub_tests COMMAND ove_test_stub)
if(OVE_TEST_BUILD_ASAN)
    add_test(NAME ove_stub_tests_asan COMMAND ove_test_stub_asan)
    # libasan's leak-detector is noisy against CMocka's intentional allocations
    # in the suite group teardown; default to off and let CI flip it back on.
    set_tests_properties(ove_stub_tests_asan PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stats=0")
endif()

# --- JUnit XML harness for CI ---
# Two independent XMLs are produced:
#   1. CMocka per-test XML: fine-grained pass/fail for every assert group.
#      CMocka 1.1.7 expands %g in CMOCKA_XML_FILE to the group-array name.
#      Our suites currently all use the C array name `tests`, so every
#      group overwrites the same file and only the *last* suite's XML
#      survives; that is good enough for CI pass/fail signal but not for
#      coverage-style reporting. Renaming the arrays is Tier 3 work.
#   2. CTest-level JUnit (requires CMake 3.21+): one case per ctest entry,
#      always usable, coarse-grained.
set(OVE_TEST_JUNIT_DIR ${CMAKE_BINARY_DIR}/junit)
add_custom_target(junit
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${OVE_TEST_JUNIT_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OVE_TEST_JUNIT_DIR}
    COMMAND ${CMAKE_COMMAND} -E env
        CMOCKA_MESSAGE_OUTPUT=xml
        CMOCKA_XML_FILE=${OVE_TEST_JUNIT_DIR}/cmocka_%g.xml
        $<TARGET_FILE:ove_test_stub>
    DEPENDS ove_test_stub
    COMMENT "Running ove_test_stub with CMocka XML output -> ${OVE_TEST_JUNIT_DIR}"
    USES_TERMINAL
)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.21)
    add_custom_target(junit-ctest
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OVE_TEST_JUNIT_DIR}
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                --output-junit ${OVE_TEST_JUNIT_DIR}/ctest.xml
        DEPENDS ove_test_stub
        COMMENT "Running CTest with JUnit output -> ${OVE_TEST_JUNIT_DIR}/ctest.xml"
        USES_TERMINAL
    )
endif()

# --- Coverage report ---
if(OVE_TEST_BUILD_COVERAGE)
    ove_test_coverage_report(stub
        ove_test_stub_coverage
        ove_test_stub_coverage_zh)
endif()
