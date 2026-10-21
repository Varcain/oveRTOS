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
set(POSIX_CORE_DIR ${OVE_ROOT}/tests/host/posix)
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends/stub)
set(STUB_SOURCES
    # Core POSIX (shared with real backend)
    ${POSIX_CORE_DIR}/posix_thread.c
    ${POSIX_CORE_DIR}/posix_sync.c
    ${OVE_SRC}/ove_sync.c
    ${POSIX_CORE_DIR}/posix_queue.c
    ${OVE_SRC}/ove_queue.c
    ${POSIX_CORE_DIR}/posix_timer.c
    ${OVE_SRC}/ove_timer.c
    ${POSIX_CORE_DIR}/posix_time.c
    ${POSIX_CORE_DIR}/posix_console.c
    ${POSIX_CORE_DIR}/posix_eventgroup.c
    ${OVE_SRC}/ove_eventgroup.c
    ${POSIX_CORE_DIR}/posix_workqueue.c
    ${OVE_SRC}/ove_workqueue.c
    ${POSIX_CORE_DIR}/posix_stream.c
    ${OVE_SRC}/ove_stream.c
    ${POSIX_CORE_DIR}/posix_watchdog.c
    ${OVE_ROOT}/backends/common/ove_nvs_ram.c
    ${OVE_ROOT}/backends/common/ove_shell.c
    ${POSIX_CORE_DIR}/posix_fs.c
    ${OVE_SRC}/ove_fs.c
    ${POSIX_CORE_DIR}/posix_block.c
    ${OVE_SRC}/ove_media.c
    ${OVE_SRC}/ove_block.c
    ${OVE_SRC}/ove_net.c
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
    # The LXP module itself is tested in isolation in its repo. The oveRTOS host
    # test compiles only the provider gate used by the consumer-side storage glue.
    ${OVE_ROOT}/backends/common/lxp_ove_net_adapter.c
    ${OVE_ROOT}/backends/common/lxp_ove_storage_adapter.c
    ${OVE_ROOT}/modules/lxp/src/lxp_async_gate.c
    ${OVE_ROOT}/backends/common/ove_net_ready.c
    ${OVE_ROOT}/backends/common/lxp_ove_host.c
    ${OVE_ROOT}/backends/common/lxp_ove_observability.c
    ${OVE_ROOT}/backends/common/lxp_ove_rt_scope.c
    ${OVE_ROOT}/backends/common/lxp_ove_console.c
    ${OVE_ROOT}/backends/common/lxp_ove_console_native.c
    ${OVE_ROOT}/backends/common/ove_lxp_metrics.c
    ${OVE_ROOT}/modules/lxp/src/lxp_rt_metrics.c
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
        # CONFIG_OVE_LINUX_NET gates the LXP net adapter
        # (backends/common/lxp_ove_net_adapter.c),
        # the one lxp-touching TU the host test compiles. The lxp module sources + their
        # LXP_ENABLE_* gates live in the lxp repo's own test build now, not here.
        CONFIG_OVE_LINUX_NET=1
        CONFIG_OVE_LINUX_LATENCY=1
        CONFIG_OVE_LINUX_FS=1
        CONFIG_OVE_BLOCK=1
        CONFIG_OVE_LINUX_BLOCK=1
        CONFIG_OVE_LINUX_BLOCK_WRITE=1
        CONFIG_OVE_LINUX_FS_SERVER_PERIOD_US=1000
        CONFIG_OVE_LINUX_FS_SERVER_BUDGET_US=300
        CONFIG_OVE_NET_RX_READY_NOTIFY=1
        LXP_ENABLE_RT_METRICS=1
        LXP_ENABLE_LATENCY=1
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

# --- Observability ring tests (tests/obs/) — standalone, no CMocka, so the
#     multi-producer cases also run under ThreadSanitizer. Uses its own
#     minimal ove_config.h from tests/obs/.
function(ove_add_obs_test name)
    add_executable(${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_trace_ring.c
        ${OVE_ROOT}/backends/common/ove_trace_ring.c
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/obs
        ${OVE_ROOT}/backends/common
    )
    target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
    target_link_libraries(${name} PRIVATE pthread)
    if(ARGN)
        target_compile_options(${name} PRIVATE ${ARGN})
        target_link_options(${name} PRIVATE ${ARGN})
    endif()
endfunction()
ove_add_obs_test(ove_test_trace_ring)

# Public user instrumentation API: name table + mark/zone records.
add_executable(ove_test_trace_api
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_trace_api.c
    ${OVE_ROOT}/backends/common/ove_trace.c
    ${OVE_ROOT}/backends/common/ove_trace_ring.c
)
target_include_directories(ove_test_trace_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs
    ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/common
    ${OVE_ROOT}/tests/host/posix/include
)
target_compile_definitions(ove_test_trace_api PRIVATE _GNU_SOURCE)

# Concurrency regression test for the user trace name table: the snapshot
# the reader gets must never contain a half-written entry.  A TSan variant
# is added below with the ring's.
function(ove_add_trace_name_concurrency_test name)
    add_executable(${name}
        ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_trace_name_concurrency.c
        ${OVE_ROOT}/backends/common/ove_trace.c
        ${OVE_ROOT}/backends/common/ove_trace_ring.c
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/obs
        ${OVE_ROOT}/include
        ${OVE_ROOT}/backends/common
        ${OVE_ROOT}/tests/host/posix/include
    )
    target_compile_definitions(${name} PRIVATE _GNU_SOURCE)
    if(ARGN)
        target_compile_options(${name} PRIVATE ${ARGN})
        target_link_options(${name} PRIVATE ${ARGN})
    endif()
    target_link_libraries(${name} PRIVATE pthread)
endfunction()
ove_add_trace_name_concurrency_test(ove_test_trace_name_concurrency)

# Cortex-M stack-scan unwinder on synthetic stacks (host-portable C).
add_executable(ove_test_arm_backtrace
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_arm_backtrace.c
    ${OVE_ROOT}/backends/common/ove_arm_backtrace.c
)
target_include_directories(ove_test_arm_backtrace PRIVATE ${OVE_INCLUDE})
# Division-free Q32 scaling of the SysTick time sources vs exact division.
add_executable(ove_test_systick_q32 ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_systick_q32.c)
target_include_directories(ove_test_systick_q32 PRIVATE ${OVE_ROOT}/backends/common)
# Per-state snapshot the RTOS backends report from ove_thread_list().
add_executable(ove_test_state_track_read ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_state_track_read.c)
target_include_directories(ove_test_state_track_read PRIVATE ${OVE_INCLUDE})
# Guest side of the sim shared-memory event ring: never overwrite unread
# events, refuse and count what does not fit.  The shim directory comes
# first, so its ove_config.h, ove/thread.h and semihosting.h are used.
add_executable(ove_test_sim_shm_backpressure ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_sim_shm_backpressure.c)
target_include_directories(ove_test_sim_shm_backpressure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/sim_shm
    ${OVE_ROOT}/sim/src
    ${OVE_ROOT}/sim/include
    ${OVE_INCLUDE}
)
# Kernel-thread state table: slots go only to emitted transitions and are
# freed on exit.  The shim's ove/thread.h comes first.
add_executable(ove_test_trace_kthread
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_trace_kthread.c
    ${OVE_ROOT}/backends/common/ove_trace.c
)
target_include_directories(ove_test_trace_kthread PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/trace_kthread
    ${CMAKE_CURRENT_SOURCE_DIR}/obs
    ${OVE_INCLUDE}
    ${OVE_ROOT}/backends/common
    ${CMAKE_CURRENT_SOURCE_DIR}/host/posix/include
)
# Profiler drain against a transport that refuses events: refused samples
# are reported once and stay out of the ring's overflow counter.
add_executable(ove_test_sim_profiler_refused
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_sim_profiler_refused.c
    ${OVE_ROOT}/sim/hal/sim_profiler.c
    ${OVE_ROOT}/backends/common/ove_profiler_ring.c
)
target_include_directories(ove_test_sim_profiler_refused PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/sim_profiler
    ${OVE_INCLUDE}
    ${OVE_ROOT}/sim/include
    ${OVE_ROOT}/backends/common
)

# NuttX scheduler-note driver: the exit of a tracked ove thread must not
# create a lane of its own, and a wake must show as READY before RUNNING.
# The observation code is not copied into the test - it is extracted
# verbatim from the backend at build time, so the test always exercises
# the real source text of backends/nuttx/nuttx_thread.c.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NUTTX_OBS_BLOCK ${CMAKE_BINARY_DIR}/generated/ove_nuttx_obs_block.inc)
add_custom_command(
    OUTPUT ${NUTTX_OBS_BLOCK}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_nuttx_obs_block.py
            ${OVE_ROOT}/backends/nuttx/nuttx_thread.c
            ${NUTTX_OBS_BLOCK}
    DEPENDS ${OVE_ROOT}/backends/nuttx/nuttx_thread.c
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_nuttx_obs_block.py
    COMMENT "Extracting the NuttX scheduler-note observation block"
    VERBATIM
)
add_executable(ove_test_nuttx_note_exit
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_note/test_nuttx_note_exit.c
    ${NUTTX_OBS_BLOCK}
    ${OVE_ROOT}/backends/common/ove_trace.c
    ${OVE_ROOT}/backends/common/ove_trace_ring.c
)
target_include_directories(ove_test_nuttx_note_exit PRIVATE
    # First: the test's own ove_config.h selects the NuttX storage header
    # and the fake <nuttx/*.h> stand-ins of the NuttX storage header.
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_note
    ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/common
    ${OVE_ROOT}/backends/nuttx/include
    ${CMAKE_BINARY_DIR}/generated
)
target_compile_definitions(ove_test_nuttx_note_exit PRIVATE _GNU_SOURCE)
target_link_libraries(ove_test_nuttx_note_exit PRIVATE pthread)

add_executable(ove_test_nuttx_note_ready
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_note/test_nuttx_note_ready.c
    ${NUTTX_OBS_BLOCK}
    ${OVE_ROOT}/backends/common/ove_trace.c
    ${OVE_ROOT}/backends/common/ove_trace_ring.c
)
target_include_directories(ove_test_nuttx_note_ready PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_note
    ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/common
    ${OVE_ROOT}/backends/nuttx/include
    ${CMAKE_BINARY_DIR}/generated
)
target_compile_definitions(ove_test_nuttx_note_ready PRIVATE _GNU_SOURCE)
target_link_libraries(ove_test_nuttx_note_ready PRIVATE pthread)

# NuttX suspend/resume handshake: a resume that races the suspend must not
# be lost - the target would park forever.  The handshake code is not
# copied into the test - it is extracted verbatim from the backend at build
# time, so the test always exercises the real source text of
# backends/nuttx/nuttx_thread.c.
set(NUTTX_SUSPEND_BLOCK ${CMAKE_BINARY_DIR}/generated/ove_nuttx_suspend_block.inc)
add_custom_command(
    OUTPUT ${NUTTX_SUSPEND_BLOCK}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_nuttx_suspend_block.py
            ${OVE_ROOT}/backends/nuttx/nuttx_thread.c
            ${NUTTX_SUSPEND_BLOCK}
    DEPENDS ${OVE_ROOT}/backends/nuttx/nuttx_thread.c
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/extract_nuttx_suspend_block.py
    COMMENT "Extracting the NuttX suspend/resume handshake block"
    VERBATIM
)
add_executable(ove_test_nuttx_suspend_handshake
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_suspend/test_nuttx_suspend_handshake.c
    ${NUTTX_SUSPEND_BLOCK}
)
target_include_directories(ove_test_nuttx_suspend_handshake PRIVATE
    # First: the test's own ove_config.h selects the NuttX storage header
    # and the fake <nuttx/*.h> stand-ins of the NuttX storage header.
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/nuttx_suspend
    ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/nuttx/include
    ${CMAKE_BINARY_DIR}/generated
)
target_compile_definitions(ove_test_nuttx_suspend_handshake PRIVATE _GNU_SOURCE)
target_link_libraries(ove_test_nuttx_suspend_handshake PRIVATE pthread)

option(OVE_TEST_BUILD_TSAN "Build ThreadSanitizer variants of the obs ring tests" ON)
# TSan cannot share a binary with ASan or MSan: when the whole tree is
# configured with either (make test-stub-sanitize, test-stub-msan), skip
# the TSan variant instead of failing the build.
set(OVE_TEST_TSAN_ACTIVE ${OVE_TEST_BUILD_TSAN})
if(CMAKE_C_FLAGS MATCHES "-fsanitize=[^ ]*(address|memory)")
    set(OVE_TEST_TSAN_ACTIVE OFF)
endif()
if(OVE_TEST_TSAN_ACTIVE)
    ove_add_obs_test(ove_test_trace_ring_tsan -fsanitize=thread -O1 -g)
    ove_add_trace_name_concurrency_test(ove_test_trace_name_concurrency_tsan
        -fsanitize=thread -O1 -g)
endif()

# Deterministic preemption of the trace ring push: trace_ring_preempt.c
# builds the ring with a hook before every atomic load, so the test can
# run a nested push and drain at an exact point of one push.
add_executable(ove_test_trace_ring_preempt
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/test_trace_ring_preempt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/obs/trace_ring_preempt.c
)
target_include_directories(ove_test_trace_ring_preempt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/obs
    ${OVE_ROOT}/backends/common
)
target_compile_definitions(ove_test_trace_ring_preempt PRIVATE CONFIG_OVE_TRACE_RING_SIZE=8)

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
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME ove_stub_tests COMMAND ove_test_stub)
add_test(NAME ove_trace_ring COMMAND ove_test_trace_ring)
add_test(NAME ove_trace_ring_preempt COMMAND ove_test_trace_ring_preempt)
add_test(NAME ove_trace_api COMMAND ove_test_trace_api)
add_test(NAME ove_trace_name_concurrency COMMAND ove_test_trace_name_concurrency)
add_test(NAME ove_arm_backtrace COMMAND ove_test_arm_backtrace)
add_test(NAME ove_systick_q32 COMMAND ove_test_systick_q32)
add_test(NAME ove_state_track_read COMMAND ove_test_state_track_read)
add_test(NAME ove_sim_shm_backpressure COMMAND ove_test_sim_shm_backpressure)
add_test(NAME ove_sim_profiler_refused COMMAND ove_test_sim_profiler_refused)
add_test(NAME ove_trace_kthread COMMAND ove_test_trace_kthread)
add_test(NAME ove_nuttx_note_exit COMMAND ove_test_nuttx_note_exit)
add_test(NAME ove_nuttx_note_ready COMMAND ove_test_nuttx_note_ready)
add_test(NAME ove_nuttx_suspend_handshake COMMAND ove_test_nuttx_suspend_handshake)
if(OVE_TEST_TSAN_ACTIVE)
    add_test(NAME ove_trace_ring_tsan COMMAND ove_test_trace_ring_tsan)
    add_test(NAME ove_trace_name_concurrency_tsan
        COMMAND ove_test_trace_name_concurrency_tsan)
    set_tests_properties(ove_trace_ring_tsan ove_trace_name_concurrency_tsan
        PROPERTIES ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")
endif()
add_test(NAME ove_patch_worktree_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_patch_worktree.py)
# The `ove test` NuttX build paths must compile the backend against the
# patched kernel (the .ready note op only exists after the
# config/patches/nuttx layer).  Static only - no NuttX checkout, no
# toolchain, no network.
add_test(NAME ove_nuttx_patch_layers_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_nuttx_patch_layers.py)
add_test(NAME ove_freertos_build_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_freertos_build.py)
add_test(NAME ove_nuttx_build_tree_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_nuttx_build_tree.py)
add_test(NAME ove_allconfigs_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_allconfigs.py)
add_test(NAME ove_app_new_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_app_new.py)
add_test(NAME ove_profiler_bridge_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_profiler_bridge.py)
# The profiler's sampling interrupt must outrank the driver interrupts whose
# time it attributes (an interrupt of equal priority cannot preempt another
# one) from a level the kernel's critical sections still mask.
add_test(NAME ove_profiler_sampler_priority_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_profiler_sampler_priority.py)
add_test(NAME ove_zig_struct_mirrors_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_zig_struct_mirrors.py)
add_test(NAME ove_bridge_trace_descriptors_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_bridge_trace_descriptors.py)
add_test(NAME ove_sim_invariants_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_sim_invariants.py)
add_test(NAME ove_manifest_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_manifest.py)
# The sim drain thread must preempt every application thread, so its
# priority has to be strictly above the highest one an application uses.
add_test(NAME ove_sim_drain_priority_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_sim_drain_priority.py)
# Every queue backend must bracket its blocking send and receive with
# the QUEUE wait markers, so a queue wait shows up on every backend.
add_test(NAME ove_queue_trace_markers_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_queue_trace_markers.py)
add_test(NAME ove_test_env_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_ove_test_env.py)
add_test(NAME ove_venv_python
    COMMAND ${CMAKE_COMMAND} -E env
        PYTHONPATH=${OVE_ROOT}/config/ove-cli
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_venv.py)
add_test(NAME ove_makefile_python_resolution_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_makefile_python_resolution.py)
add_test(NAME ove_rust_trace_cfg_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_rust_trace_cfg.py)
add_test(NAME ove_config_matrix_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_config_matrix.py)
add_test(NAME ove_ci_workflow_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_ci_workflow.py)
# docs-site/docs/build-system/sim.md must agree with the code: the profiler
# ceiling, the trace/profiler wire formats, the plugin vtable and the
# Kconfig defaults.
add_test(NAME ove_sim_docs_consistency_python
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/python/test_sim_docs_consistency.py)
add_test(NAME ove_build_type_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestBuildType.cmake)
add_test(NAME ove_arm_float_abi_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestArmFloatAbi.cmake)
add_test(NAME ove_freertos_bootstrap_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestFreeRTOSBootstrap.cmake)
add_test(NAME ove_lxp_wx_config_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestLxpWxConfig.cmake)
add_test(NAME ove_lxp_port_ownership_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestLxpPortOwnership.cmake)
add_test(NAME ove_linux_interop_ergonomics_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestLinuxInteropErgonomics.cmake)
add_test(NAME ove_backend_filter_cmake
    COMMAND ${CMAKE_COMMAND}
        -DOVE_ROOT=${OVE_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestBackendFilter.cmake)
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
