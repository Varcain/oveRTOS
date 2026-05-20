# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)

set(CMAKE_TOOLCHAIN_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake
    CACHE FILEPATH "ARM toolchain file")

project(ove_test_freertos_qemu_zeroheap C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
                    -ffunction-sections -fdata-sections)

include(FetchContent)

# --- Fetch FreeRTOS-Kernel ---
FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(freertos_kernel)
if(NOT freertos_kernel_POPULATED)
    FetchContent_Populate(freertos_kernel)
endif()

# --- Fetch CMocka (source only, no cmake build — bare-metal lacks signals) ---
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
# Compile cmocka.c directly with bare-metal-safe defines
add_library(cmocka-static STATIC ${cmocka_SOURCE_DIR}/src/cmocka.c)
target_include_directories(cmocka-static PUBLIC ${cmocka_SOURCE_DIR}/include)
target_include_directories(cmocka-static PRIVATE ${cmocka_SOURCE_DIR}/src)
target_compile_definitions(cmocka-static PRIVATE
    HAVE_STRINGS_H HAVE_INTTYPES_H HAVE_MALLOC_H
    HAVE_SIGNAL_H
    HAVE_SETJMP_H HAVE_STDARG_H HAVE_STDDEF_H HAVE_STDINT_H
    HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
    HAVE_CALLOC HAVE_EXIT HAVE_FREE HAVE_LONGJMP HAVE_MALLOC
    HAVE_MEMCPY HAVE_MEMSET HAVE_SETJMP HAVE_STRCMP HAVE_SPRINTF
    HAVE_VSNPRINTF HAVE_FPRINTF HAVE_PRINTF
)
# Bare-metal newlib has signal.h with SIGFPE/etc but signal() is a stub;
# suppress the implicit-function-declaration error for signal()
target_compile_options(cmocka-static PRIVATE
    -Wno-unused-parameter -Wno-implicit-function-declaration -Wno-int-conversion)

# --- Paths ---
set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)
set(FREERTOS_BACKEND ${OVE_ROOT}/backends/freertos)
set(TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STUB_DIR ${TESTS_ROOT}/backends/stub)
set(SUITE_DIR ${TESTS_ROOT}/suites)
include(${TESTS_ROOT}/cmake/OveTest.cmake)
set(BOARD_DIR ${OVE_ROOT}/boards/qemu-mps2-an500)
set(FREERTOS_SRC ${freertos_kernel_SOURCE_DIR})
set(FREERTOS_PORT ${FREERTOS_SRC}/portable/GCC/ARM_CM7/r0p1)

# --- CMSIS startup and system ---
# syscalls.c (legacy newlib _sbrk against __HeapLimit) removed with the
# picolibc migration — picolibc provides its own picosbrk/_sbrk against
# linker-script __heap_start/__heap_end.
set(STARTUP_SOURCES
    ${BOARD_DIR}/freertos/startup_CMSDK_CM7.s
    ${BOARD_DIR}/freertos/system_CMSDK_CM7.c
)

# --- FreeRTOS kernel sources (heap stubs instead of heap_4.c) ---
set(FREERTOS_KERNEL_SOURCES
    ${FREERTOS_SRC}/tasks.c
    ${FREERTOS_SRC}/queue.c
    ${FREERTOS_SRC}/list.c
    ${FREERTOS_SRC}/timers.c
    ${FREERTOS_SRC}/event_groups.c
    ${FREERTOS_SRC}/stream_buffer.c
    ${FREERTOS_BACKEND}/freertos_heap_stubs.c
    ${FREERTOS_PORT}/port.c
)

# --- Pure FreeRTOS backend modules (7, direct-linked) ---
set(FREERTOS_BACKEND_SOURCES
    ${FREERTOS_BACKEND}/freertos_thread.c
    # Heap-lock wrap chain (zero-heap only).  Common impl in
    # backends/common/ove_heap_lock.c; freertos extras here.
    ${OVE_ROOT}/backends/common/ove_heap_lock.c
    ${FREERTOS_BACKEND}/freertos_heap_lock.c
    ${FREERTOS_BACKEND}/freertos_sync.c
    ${FREERTOS_BACKEND}/freertos_queue.c
    ${FREERTOS_BACKEND}/freertos_timer.c
    ${FREERTOS_BACKEND}/freertos_eventgroup.c
    ${FREERTOS_BACKEND}/freertos_stream.c
    ${FREERTOS_BACKEND}/freertos_workqueue.c
    # Async runtime substrate (CONFIG_OVE_ASYNC).
    ${FREERTOS_BACKEND}/freertos_irq.c
)

# --- Core modules (board/GPIO/LED dispatcher) ---
set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
)

# --- Stub backend for non-FreeRTOS modules ---
set(STUB_SOURCES
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    ${STUB_DIR}/stub_bsp.c
    ${STUB_DIR}/stub_lvgl.c
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_time.c
    ${STUB_DIR}/stub_console.c
    ${STUB_DIR}/stub_watchdog.c
    ${STUB_DIR}/stub_shell.c
    ${STUB_DIR}/stub_nvs.c
)

# --- Test suites (list sourced from cmake/OveTest.cmake) ---
ove_test_common_suite_sources(SUITE_SOURCES ${SUITE_DIR})

# --- Executable ---
add_executable(ove_test_freertos_qemu_zeroheap
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_main.c
    ${STARTUP_SOURCES}
    ${FREERTOS_KERNEL_SOURCES}
    ${FREERTOS_BACKEND_SOURCES}
    ${CORE_SOURCES}
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_compile_definitions(ove_test_freertos_qemu_zeroheap PRIVATE
    OVE_QEMU_ARM
    CONFIG_OVE_ZERO_HEAP=1
)

target_include_directories(ove_test_freertos_qemu_zeroheap PRIVATE
    # FreeRTOSConfig.h and ove_config.h live here
    ${CMAKE_CURRENT_SOURCE_DIR}
    # FreeRTOS kernel headers
    ${FREERTOS_SRC}/include
    # FreeRTOS ARM_CM7/r0p1 port
    ${FREERTOS_PORT}
    # oveRTOS public API
    ${OVE_INCLUDE}
    # FreeRTOS backend storage headers (ove_storage_freertos.h)
    ${FREERTOS_BACKEND}/include
    # backends/common (ove_backend_common.h)
    ${OVE_ROOT}/backends/common
    # Test framework and stub backend
    ${TESTS_ROOT}
)

target_link_libraries(ove_test_freertos_qemu_zeroheap PRIVATE
    cmocka-static
    m
)

# Linker script
set(LINKER_SCRIPT ${BOARD_DIR}/freertos/mps2_an500.ld)
target_link_options(ove_test_freertos_qemu_zeroheap PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections
)

# FreeRTOS kernel files generate some warnings with -Wextra; suppress for those
set_source_files_properties(${FREERTOS_KERNEL_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare"
)

# Zero-heap link-time wrap + audit — see renode-stm32f746-freertos-zeroheap.
include(${OVE_ROOT}/cmake/OveZeroHeapAudit.cmake)
ove_apply_zero_heap_wrap(ove_test_freertos_qemu_zeroheap)
ove_zero_heap_assert_no_kernel_alloc(ove_test_freertos_qemu_zeroheap
    RTOS FREERTOS STRICT)

# Dispatch-overhead symbol audit + panic-symbol guard (zeroheap adds
# NO_PANIC_SYMBOLS to catch Rust/Zig panic-formatting that would pull
# in allocating fmt paths). Symbol audit only — no hotpath disasm on
# test ELFs (see freertos-qemu/CMakeLists.txt rationale).
include(${OVE_ROOT}/cmake/OveZeroOverheadAudit.cmake)
ove_assert_no_dispatch_overhead(ove_test_freertos_qemu_zeroheap
    BINDING c NO_PANIC_SYMBOLS)
