# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# oveRTOS - RTOS Abstraction Framework
# Add as a subdirectory from the parent project's CMakeLists.txt:
#
#     add_subdirectory(path/to/oveRTOS)
#     target_link_libraries(my_firmware PRIVATE ove)
#
# Linking against the `ove` INTERFACE target pulls in the core sources
# and include path. Backend sources (FreeRTOS / Zephyr / NuttX / POSIX)
# are added separately by including the per-RTOS Ove*.cmake helpers
# from the board CMakeLists.txt.

cmake_minimum_required(VERSION 3.16)

set(OVE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(_OVE_CORE_SOURCES
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

# Modern interface target — survives function() / nested scopes that
# would silently lose PARENT_SCOPE variables.
if(NOT TARGET ove)
    add_library(ove INTERFACE)
    target_sources(ove INTERFACE ${_OVE_CORE_SOURCES})
    target_include_directories(ove INTERFACE ${OVE_DIR}/include)
endif()

# Legacy variables — kept only for parent projects still on the old
# `add_subdirectory + ${OVE_SOURCES}` pattern. New consumers should
# `target_link_libraries(<tgt> PRIVATE ove)` instead.
set(OVE_SOURCES ${_OVE_CORE_SOURCES})
set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
set(OVE_BACKENDS_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends/common)

# Only export PARENT_SCOPE when actually used as a subdirectory, not
# when this file is processed as a standalone project (e.g. from CLion
# pointing at the repo root).
get_directory_property(_ove_has_parent PARENT_DIRECTORY)
if(_ove_has_parent)
    set(OVE_SOURCES ${OVE_SOURCES} PARENT_SCOPE)
    set(OVE_INCLUDE_DIR ${OVE_INCLUDE_DIR} PARENT_SCOPE)
    set(OVE_BACKENDS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/backends PARENT_SCOPE)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_BACKENDS_COMMON_DIR} PARENT_SCOPE)
    set(OVE_DRIVERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/drivers PARENT_SCOPE)
endif()
