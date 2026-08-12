# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_tests C)

# Emit compile_commands.json so `ove lint` can feed clang-tidy real -I/-D
# flags for the suite + backend sources.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Fail configure early if the category lists drift from disk reality
# (file added but not listed, listed but missing, listed twice).
ove_test_validate_suite_membership(${CMAKE_CURRENT_SOURCE_DIR}/suites)

# Also fail if the cmake category lists drift from the runtime dispatch
# categories in framework/suites.inc (e.g. COMMON in one, STUB in the other).
ove_test_validate_suites_inc_parity(${CMAKE_CURRENT_SOURCE_DIR}/framework/suites.inc)

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
    ${POSIX_CORE_DIR}/posix_block.c
    ${POSIX_CORE_DIR}/posix_net.c
    ${POSIX_CORE_DIR}/posix_protected.c
    # Async runtime substrate (compiled to no-ops when CONFIG_OVE_ASYNC=n).
    ${POSIX_CORE_DIR}/posix_irq.c
    # Shared BSP modules
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    # Audio graph engine (platform-independent)
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    # Arena allocator (platform-independent)
    ${OVE_ROOT}/backends/common/ove_arena.c
    # Module loader (platform-independent)
    ${OVE_ROOT}/loader/ove_loader.c
    # Host adapters for LXP's handle-based network port and thread snapshots.
    # The lxp module itself is tested in isolation in the lxp repo; the oveRTOS
    # host test covers only the consumer-side glue, so no lxp sources are built.
    ${OVE_ROOT}/backends/common/lxp_ove_adapter.c
    ${OVE_ROOT}/backends/common/lxp_ove_fs_adapter.c
    ${OVE_ROOT}/backends/common/ove_net_ready.c
    ${OVE_ROOT}/backends/common/lxp_ove_host.c
    ${OVE_ROOT}/backends/common/ove_lxp_metrics.c
    # Explicit thread-snapshot type translation at the oveRTOS/LXP boundary.
    ${OVE_ROOT}/backends/common/lxp_ove_thread_adapter.c
    # Heap-lock machinery: portable wrappers + stub-only weak __real_*
    # fallbacks (analogue of {freertos,zephyr,nuttx}_heap_lock.c).
    ${OVE_ROOT}/backends/common/ove_heap_lock.c
    ${STUB_DIR}/stub_heap_lock.c
    # Power management (platform-independent)
    ${OVE_SRC}/ove_pm.c
    # Headless stubs (display, HAL)
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    ${STUB_DIR}/stub_pm.c
    # Bus-driver public API + stub HAL impls.  The .c files are gated
    # by CONFIG_OVE_{I2C,SPI,UART}, which is enabled per-target below
    # (kept off the global tests/ove_config.h to avoid disturbing the
    # Rust-bindings sizeof/alignof probe — same rationale as
    # CONFIG_OVE_NET).
    ${OVE_SRC}/ove_i2c.c
    ${OVE_SRC}/ove_spi.c
    ${OVE_SRC}/ove_uart.c
    ${STUB_DIR}/stub_i2c.c
    ${STUB_DIR}/stub_spi.c
    ${STUB_DIR}/stub_uart.c
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

# The loader suite needs a real relocatable object to load. Compile
# suites/loader_mod.c to a freestanding ET_REL .o (absolute relocations via
# -mcmodel=large -fno-pic) and embed it as a byte array the suite #includes.
set(LOADER_MOD_GEN ${CMAKE_BINARY_DIR}/generated)
set(LOADER_MOD_HDR ${LOADER_MOD_GEN}/loader_mod_image.h)
add_custom_command(
    OUTPUT ${LOADER_MOD_HDR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${LOADER_MOD_GEN}
    COMMAND ${CMAKE_C_COMPILER}
            -O2 -fno-pic -mcmodel=large -ffreestanding
            -fno-stack-protector -fno-asynchronous-unwind-tables
            -c ${SUITE_DIR}/loader_mod.c -o ${LOADER_MOD_GEN}/loader_mod.o
    COMMAND ${CMAKE_COMMAND} -DIN=${LOADER_MOD_GEN}/loader_mod.o
            -DOUT=${LOADER_MOD_HDR} -DSYM=ove_loader_test_mod
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
    DEPENDS ${SUITE_DIR}/loader_mod.c ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
    COMMENT "Building + embedding loader test module"
    VERBATIM
)
set(LOADER_MOD_DEPS ${LOADER_MOD_HDR})

# If an ARM cross-compiler is available, also build a real ELF32/ARM module so
# the loader's ARM relocation path is exercised on the host (verified by value;
# ARM code is not executed here).
find_program(OVE_ARM_GCC arm-none-eabi-gcc)
if(OVE_ARM_GCC)
    set(LOADER_MOD_ARM_HDR ${LOADER_MOD_GEN}/loader_mod_arm_image.h)
    add_custom_command(
        OUTPUT ${LOADER_MOD_ARM_HDR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${LOADER_MOD_GEN}
        COMMAND ${OVE_ARM_GCC}
                -mcpu=cortex-m4 -mthumb -O2 -fno-pic -ffreestanding
                -fno-asynchronous-unwind-tables
                -c ${SUITE_DIR}/loader_mod_arm.c -o ${LOADER_MOD_GEN}/loader_mod_arm.o
        COMMAND ${CMAKE_COMMAND} -DIN=${LOADER_MOD_GEN}/loader_mod_arm.o
                -DOUT=${LOADER_MOD_ARM_HDR} -DSYM=ove_loader_test_mod_arm
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
        DEPENDS ${SUITE_DIR}/loader_mod_arm.c ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_bin.cmake
        COMMENT "Building + embedding ARM loader test module"
        VERBATIM
    )
    list(APPEND LOADER_MOD_DEPS ${LOADER_MOD_ARM_HDR})
endif()
add_custom_target(loader_mod_gen DEPENDS ${LOADER_MOD_DEPS})

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
        # lxp public headers — the adapter + test_lxp_adapter.c include lxp/lxp_net_ops.h.
        ${OVE_ROOT}/modules/lxp/include
        ${OVE_ROOT}/backends/common
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${POSIX_CORE_DIR}/include
        ${cmocka_SOURCE_DIR}/include
        ${LOADER_MOD_GEN}
    )
    add_dependencies(${name} loader_mod_gen)
    target_link_libraries(${name} PRIVATE cmocka-static pthread rt m)
    # Enable the socket / bus APIs for the stub test target only.  See
    # tests/ove_config.h for why this is per-target rather than global.
    target_compile_definitions(${name} PRIVATE
        CONFIG_OVE_NET=1
        # CONFIG_OVE_LINUX_NET gates the lxp net adapter (backends/common/lxp_ove_adapter.c),
        # the one lxp-touching TU the host test compiles. The lxp module sources + their
        # LXP_ENABLE_* gates live in the lxp repo's own test build now, not here.
        CONFIG_OVE_LINUX_NET=1
        CONFIG_OVE_LINUX_FS=1
        CONFIG_OVE_BLOCK=1
        CONFIG_OVE_LINUX_BLOCK=1
        CONFIG_OVE_LINUX_BLOCK_WRITE=1
        CONFIG_OVE_LINUX_FS_SERVER_PERIOD_US=1000
        CONFIG_OVE_LINUX_FS_SERVER_BUDGET_US=300
        CONFIG_OVE_NET_RX_READY_NOTIFY=1
        CONFIG_OVE_I2C=1
        CONFIG_OVE_SPI=1
        CONFIG_OVE_UART=1
    )
    if(OVE_ARM_GCC)
        target_compile_definitions(${name} PRIVATE OVE_TEST_HAVE_ARM=1)
    endif()
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
add_test(NAME ove_arm_float_abi_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestArmFloatAbi.cmake)
add_test(NAME ove_lxp_wx_config_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestLxpWxConfig.cmake)
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
