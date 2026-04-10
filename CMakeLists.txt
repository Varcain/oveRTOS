# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# oveRTOS - RTOS Abstraction Framework
# Add as a subdirectory from the parent project's CMakeLists.txt

set(OVE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(OVE_SOURCES
    ${OVE_DIR}/src/ove_app.c
    ${OVE_DIR}/src/ove_thread.c
    ${OVE_DIR}/src/ove_sync.c
    ${OVE_DIR}/src/ove_audio.c
    ${OVE_DIR}/src/ove_fs.c
    ${OVE_DIR}/src/ove_queue.c
    ${OVE_DIR}/src/ove_timer.c
    ${OVE_DIR}/src/ove_console.c
    ${OVE_DIR}/src/ove_time.c
    ${OVE_DIR}/src/ove_bsp.c
    ${OVE_DIR}/src/ove_lvgl.c
    ${OVE_DIR}/src/ove_eventgroup.c
    ${OVE_DIR}/src/ove_workqueue.c
    ${OVE_DIR}/src/ove_stream.c
    ${OVE_DIR}/src/ove_watchdog.c
    ${OVE_DIR}/src/ove_nvs.c
    ${OVE_DIR}/src/ove_shell.c
    ${OVE_DIR}/src/ove_uart.c
    ${OVE_DIR}/src/ove_spi.c
    ${OVE_DIR}/src/ove_i2c.c
    ${OVE_DIR}/src/ove_i2s.c
    ${OVE_DIR}/src/ove_pm.c
)

set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
set(OVE_BACKENDS_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends/common)

# When used as add_subdirectory(), export sources and include path
# to parent scope for integration into firmware builds.
# (Embedded projects typically compile all sources into one target
# rather than linking separate static libraries.)
set(OVE_SOURCES ${OVE_SOURCES} PARENT_SCOPE)
set(OVE_INCLUDE_DIR ${OVE_INCLUDE_DIR} PARENT_SCOPE)
set(OVE_BACKENDS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends PARENT_SCOPE)
set(OVE_BACKENDS_COMMON_DIR ${OVE_BACKENDS_COMMON_DIR} PARENT_SCOPE)
set(OVE_DRIVERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/drivers PARENT_SCOPE)
