# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# oveRTOS Zephyr board CMakeLists.txt for QEMU MPS2-AN500.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZephyrCommon.cmake)

ove_zephyr_pre_find()

# DTS overlay for semihosting display (adds zephyr,display chosen node)
set(DTC_OVERLAY_FILE "${OVE_BOARD_DIR}/dts/shmem_display.overlay" CACHE STRING "" FORCE)

# Shmem display driver (writes to /dev/shm/ove-fb via semihosting)
list(APPEND ZEPHYR_EXTRA_MODULES ${OVE_BOARD_DIR}/drivers)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

# ── Sim framework (replaces semihosting display/audio drivers) ──────
# Exclude the NuttX/Zephyr audio+lvgl backends; the sim framework
# handles display + audio through the SHM guest transport.
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

# Staged lv_conf.h copy so Rust bindgen (which processes LVGL headers
# with its own clang) can find it via lv_conf_internal.h's relative
# include path.
ove_zephyr_copy_lv_conf_for_bindgen()

ove_zephyr_add_tflm_if_enabled()
ove_zephyr_include_app_sources()
ove_zephyr_add_common_includes(
    EXTRA
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
