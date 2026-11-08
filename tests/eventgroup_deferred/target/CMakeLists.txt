# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_eventgroup_deferred_target C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
option(OVE_MPU_V1 "Check native restricted-task teardown compatibility" OFF)
set(OVE_MPU_REGIONS 16 CACHE STRING "Actual QEMU MPU region count")
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
set(program main.c)
if(OVE_MPU_V1)
    set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM4_MPU")
    set(program mpu.c)
endif()
if(NOT EXISTS "${port}/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the pinned kernel")
endif()
set(OVE_DEFERRED_PROVIDER "${OVE_ROOT}/backends/freertos/freertos_eventgroup.c"
    CACHE FILEPATH "EventGroup provider body")
option(OVE_ZERO_HEAP "Use caller-owned storage only" OFF)
option(OVE_DEFERRED_LIFETIME "Exercise repaired deferred retirement" ON)
option(OVE_DEFERRED_CANARY "Retain a harmless early observation failure" OFF)
add_executable(eventgroup_target "${program}" "${OVE_DEFERRED_PROVIDER}"
    "${OVE_ROOT}/src/ove_eventgroup.c" "${OVE_ROOT}/backends/freertos/freertos_hooks.c"
    "${FREERTOS_PATH}/tasks.c" "${FREERTOS_PATH}/queue.c" "${FREERTOS_PATH}/list.c"
    "${FREERTOS_PATH}/timers.c" "${FREERTOS_PATH}/event_groups.c" "${port}/port.c"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c")
if(OVE_MPU_V1)
    target_compile_definitions(eventgroup_target PRIVATE OVE_MPU_V1=1
        OVE_MPU_REGIONS=${OVE_MPU_REGIONS})
    target_sources(eventgroup_target PRIVATE "${FREERTOS_PATH}/portable/Common/mpu_wrappers.c"
        "${FREERTOS_PATH}/stream_buffer.c")
endif()
if(OVE_ZERO_HEAP)
    target_compile_definitions(eventgroup_target PRIVATE CONFIG_OVE_ZERO_HEAP=1)
    target_sources(eventgroup_target PRIVATE "${OVE_ROOT}/backends/freertos/freertos_heap_stubs.c")
else()
    target_sources(eventgroup_target PRIVATE "${FREERTOS_PATH}/portable/MemMang/heap_4.c")
endif()
target_compile_definitions(eventgroup_target PRIVATE
    OVE_DEFERRED_LIFETIME=$<BOOL:${OVE_DEFERRED_LIFETIME}>
    OVE_DEFERRED_CANARY=$<BOOL:${OVE_DEFERRED_CANARY}>)
target_include_directories(eventgroup_target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FREERTOS_PATH}/include" "${port}" "${OVE_ROOT}/include"
    "${OVE_ROOT}/backends/freertos/include" "${OVE_ROOT}/backends/common" "${OVE_ROOT}/tests")
target_compile_options(eventgroup_target PRIVATE -Os -g3 -Wall -Wextra -Werror
    -Wno-unused-parameter -ffunction-sections -fdata-sections -fstack-usage)
target_link_options(eventgroup_target PRIVATE -Wl,--gc-sections
    "-T${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/eventgroup_target.map")
