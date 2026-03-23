# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# oveRTOS Zephyr board CMakeLists.txt for QEMU MPS2-AN500.
# Paths set by oveRTOS top-level Makefile or defaults.

cmake_minimum_required(VERSION 3.20.0)

# Path defaults
if(NOT DEFINED OVE_DIR)
    get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
if(NOT DEFINED OVE_APP_DIR)
    file(STRINGS "${OVE_DIR}/.config" _app_line REGEX "^CONFIG_OVE_APP_NAME=")
    if(_app_line)
        string(REGEX REPLACE ".*=\"(.*)\"" "\\1" _app_name "${_app_line}")
        set(OVE_APP_DIR "${OVE_DIR}/apps/${_app_name}")
    endif()
endif()
if(NOT DEFINED OVE_GEN_DIR)
    set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
endif()
set(OVE_BOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Include generated CMake config (sets OVE_RTOS, OVE_APP_LANG, etc.)
if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
    include(${OVE_GEN_DIR}/ove_config.cmake)
endif()

# DTS overlay for semihosting display (adds zephyr,display chosen node)
set(DTC_OVERLAY_FILE "${OVE_BOARD_DIR}/dts/shmem_display.overlay" CACHE STRING "" FORCE)

# DTS binding search path for ove,shmem-display compatible
list(APPEND DTS_ROOT "${OVE_BOARD_DIR}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

# Replace Zephyr audio backend with QEMU semihosting audio driver
if(OVE_AUDIO)
    list(FILTER OVE_BACKEND_SOURCES EXCLUDE REGEX "_audio\\.c$")
    list(APPEND OVE_BACKEND_SOURCES
         "${OVE_BOARD_DIR}/../src/qemu_audio.c")
endif()

target_sources(app PRIVATE
    ${OVE_BOARD_DIR}/main.c

    # oveRTOS framework + backend (only enabled modules, from ove_config.cmake)
    ${OVE_BACKEND_SOURCES}
)

# Semihosting display driver for QEMU (implements Zephyr display API)
if(CONFIG_DISPLAY)
    target_sources(app PRIVATE ${OVE_BOARD_DIR}/drivers/display_shmem.c)
endif()

# Place lv_conf.h where LVGL's lv_conf_internal.h expects it via
# relative #include "../../lv_conf.h" from lvgl/src/.  Needed for
# Rust bindgen which processes LVGL headers with its own clang.
if(DEFINED ZEPHYR_BASE)
    set(_LVGL_CONF_DEST "${ZEPHYR_BASE}/../modules/lib/gui/lv_conf.h")
    if(NOT EXISTS "${_LVGL_CONF_DEST}")
        file(COPY "${OVE_BOARD_DIR}/lv_conf.h"
             DESTINATION "${ZEPHYR_BASE}/../modules/lib/gui")
    endif()
endif()

# Application code (generated from app.yaml by 'ove configure')
if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
    include(${OVE_GEN_DIR}/app_sources.cmake)
elseif(EXISTS "${OVE_APP_DIR}/CMakeLists.txt")
    message(WARNING "Using legacy app CMakeLists.txt — migrate to app.yaml")
    include(${OVE_APP_DIR}/CMakeLists.txt)
else()
    message(FATAL_ERROR "No app build description found. Run 'ove configure' first.")
endif()
include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)
ove_apply_app_language(app)

target_include_directories(app PRIVATE
    ${OVE_GEN_DIR}
    ${APP_INCLUDE_DIRS}
    ${OVE_DIR}/include
    ${OVE_DIR}/bindings/cpp
    ${OVE_DIR}/backends/zephyr/include
    ${OVE_DIR}/backends/common
    ${OVE_BOARD_DIR}/../src
)

# Redirect malloc/free to k_malloc/k_free so allocations go through
# Zephyr's _system_heap (queryable via sys_heap_runtime_stats_get).
# Only affects ove sources — the prebuilt libc is unaffected.
target_compile_definitions(app PRIVATE
    malloc=k_malloc
    free=k_free
    calloc=k_calloc
    realloc=k_realloc
)
