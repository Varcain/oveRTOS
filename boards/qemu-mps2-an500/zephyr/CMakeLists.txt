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
include(${OVE_DIR}/cmake/OveAppResolve.cmake)
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

# Shmem display driver (writes to /dev/shm/ove-fb via semihosting)
list(APPEND ZEPHYR_EXTRA_MODULES ${OVE_BOARD_DIR}/drivers)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

# ── Sim framework (replaces semihosting display/audio drivers) ──────
# Exclude old backends; sim framework handles display + audio.
if(OVE_AUDIO)
    list(FILTER OVE_BACKEND_SOURCES EXCLUDE REGEX "_audio\\.c$")
endif()
if(OVE_LVGL)
    list(FILTER OVE_BACKEND_SOURCES EXCLUDE REGEX "_lvgl\\.c$")
endif()

set(SIM_QEMU_SOURCES
    ${OVE_DIR}/sim/src/ove_sim_registry.c
    ${OVE_DIR}/sim/src/ove_sim_input.c
    ${OVE_DIR}/sim/src/ove_sim_transport_shm_guest.c
    ${OVE_DIR}/sim/hal/sim_board.c
    ${OVE_DIR}/sim/hal/sim_display.c
)
if(OVE_LVGL)
    list(APPEND SIM_QEMU_SOURCES ${OVE_DIR}/sim/hal/sim_lvgl.c)
endif()
if(OVE_AUDIO)
    list(APPEND SIM_QEMU_SOURCES ${OVE_DIR}/sim/hal/sim_audio.c)
endif()

target_sources(app PRIVATE
    ${OVE_BOARD_DIR}/main.c
    ${OVE_BACKEND_SOURCES}
    ${SIM_QEMU_SOURCES}
)

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

# ML inference (TensorFlow Lite Micro)
if(OVE_INFER AND NOT TARGET ove_tflm)
    if(NOT DEFINED OVE_DL_DIR)
        set(OVE_DL_DIR "${OVE_DIR}/dl")
    endif()
    set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
    include(${OVE_DIR}/cmake/OveTflm.cmake)
    ove_build_tflm()
    target_link_libraries(ove_tflm PRIVATE zephyr_interface)
    zephyr_link_libraries(ove_tflm)
    target_link_libraries(app PRIVATE stdc++)
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
    ${OVE_DIR}/sim/include
    ${OVE_DIR}/sim/src
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
