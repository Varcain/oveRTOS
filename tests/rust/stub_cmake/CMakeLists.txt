# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_stub_lib C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)
set(POSIX_CORE_DIR ${OVE_ROOT}/backends/posix)
set(STUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/stub)

# Include test helper (provides ove_config.h location)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/OveTest.cmake)

add_library(ove_stub STATIC
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
    ${OVE_SRC}/ove_pm.c
    # Audio graph engine
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    # Headless stubs
    ${STUB_DIR}/stub_gpio.c
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_lvgl.c
    ${STUB_DIR}/stub_pm.c
    ${STUB_DIR}/stub_audio_device.c
)

target_include_directories(ove_stub PRIVATE
    ${OVE_INCLUDE}
    ${OVE_ROOT}/backends/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${POSIX_CORE_DIR}/include
)

# ── Generate storage type sizes for Rust bindings ──────────────────
# Compile a probe file that measures sizeof/alignof of each storage type,
# then extract the values into a KEY=VALUE .env file.
set(SIZES_C "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
set(SIZES_ENV "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")

file(WRITE ${SIZES_C}
"#include \"ove/ove.h\"\n\
#include \"ove/storage.h\"\n\
#include <stddef.h>\n\
#define S(type) unsigned char _sizeof_##type[sizeof(type)];\n\
#define A(type) unsigned char _alignof_##type[_Alignof(type)];\n\
S(ove_thread_storage_t)     A(ove_thread_storage_t)\n\
S(ove_queue_storage_t)      A(ove_queue_storage_t)\n\
S(ove_timer_storage_t)      A(ove_timer_storage_t)\n\
S(ove_mutex_storage_t)      A(ove_mutex_storage_t)\n\
S(ove_sem_storage_t)        A(ove_sem_storage_t)\n\
S(ove_event_storage_t)      A(ove_event_storage_t)\n\
S(ove_condvar_storage_t)    A(ove_condvar_storage_t)\n\
S(ove_eventgroup_storage_t) A(ove_eventgroup_storage_t)\n\
S(ove_workqueue_storage_t)  A(ove_workqueue_storage_t)\n\
S(ove_work_storage_t)       A(ove_work_storage_t)\n\
S(ove_stream_storage_t)     A(ove_stream_storage_t)\n\
S(ove_watchdog_storage_t)   A(ove_watchdog_storage_t)\n\
S(ove_file_storage_t)       A(ove_file_storage_t)\n\
S(ove_dir_storage_t)        A(ove_dir_storage_t)\n"
)

add_library(_ove_sizes OBJECT ${SIZES_C})
target_include_directories(_ove_sizes PRIVATE
    ${OVE_INCLUDE}
    ${OVE_ROOT}/backends/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${POSIX_CORE_DIR}/include
)
target_compile_options(_ove_sizes PRIVATE -w)

add_custom_command(
    OUTPUT ${SIZES_ENV}
    COMMAND python3
        ${OVE_ROOT}/config/scripts/extract_storage_sizes.py
        $<TARGET_OBJECTS:_ove_sizes> ${SIZES_ENV}
    DEPENDS _ove_sizes
            ${OVE_ROOT}/include/ove/storage.h
    COMMENT "Extracting storage type sizes for Rust bindings"
)

add_custom_target(storage_sizes ALL DEPENDS ${SIZES_ENV})
