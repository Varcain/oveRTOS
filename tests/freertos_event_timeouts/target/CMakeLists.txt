# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_event_timeout_target C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
option(OVE_MPU_V1 "Exercise restricted native caller mode" OFF)
option(OVE_ZERO_HEAP "Use caller-owned objects only" OFF)
option(OVE_NATIVE_ABORT "Enable native abort compatibility controls" OFF)
option(OVE_EVENT_TARGET_CANARY "Retain an early observation failure" OFF)
set(OVE_MPU_REGIONS 16 CACHE STRING "Native QEMU MPU region count")
set(OVE_NATIVE_TICK_BITS 32 CACHE STRING "Native supported tick width")
if(NOT OVE_NATIVE_TICK_BITS MATCHES "^(16|32)$")
    message(FATAL_ERROR "This ARM target supports only 16/32-bit native ticks")
endif()
set(OVE_EVENT_PROVIDER "${OVE_ROOT}/backends/freertos/freertos_sync.c" CACHE FILEPATH "Actual Event provider")
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
if(OVE_MPU_V1)
    set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM4_MPU")
endif()
if(NOT EXISTS "${port}/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the pinned native kernel")
endif()
add_executable(event_timeout_target main.c "${OVE_EVENT_PROVIDER}"
    "${OVE_ROOT}/backends/freertos/freertos_eventgroup.c"
    "${OVE_ROOT}/src/ove_sync.c" "${OVE_ROOT}/src/ove_eventgroup.c"
    "${OVE_ROOT}/backends/freertos/freertos_hooks.c"
    "${FREERTOS_PATH}/tasks.c" "${FREERTOS_PATH}/queue.c" "${FREERTOS_PATH}/list.c"
    "${FREERTOS_PATH}/timers.c" "${FREERTOS_PATH}/event_groups.c" "${port}/port.c"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c")
target_compile_definitions(event_timeout_target PRIVATE OVE_NATIVE_ABORT=$<BOOL:${OVE_NATIVE_ABORT}>
    OVE_NATIVE_TICK_BITS=${OVE_NATIVE_TICK_BITS}
    OVE_EVENT_TARGET_CANARY=$<BOOL:${OVE_EVENT_TARGET_CANARY}>)
if(OVE_MPU_V1)
    target_compile_definitions(event_timeout_target PRIVATE OVE_MPU_V1=1 OVE_MPU_REGIONS=${OVE_MPU_REGIONS})
    target_sources(event_timeout_target PRIVATE "${FREERTOS_PATH}/portable/Common/mpu_wrappers.c"
        "${FREERTOS_PATH}/stream_buffer.c")
endif()
if(OVE_ZERO_HEAP)
    target_compile_definitions(event_timeout_target PRIVATE CONFIG_OVE_ZERO_HEAP=1)
    target_sources(event_timeout_target PRIVATE "${OVE_ROOT}/backends/freertos/freertos_heap_stubs.c")
else()
    target_sources(event_timeout_target PRIVATE "${FREERTOS_PATH}/portable/MemMang/heap_4.c")
endif()
target_include_directories(event_timeout_target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FREERTOS_PATH}/include" "${port}" "${OVE_ROOT}/include"
    "${OVE_ROOT}/backends/freertos/include" "${OVE_ROOT}/backends/common" "${OVE_ROOT}/tests")
target_compile_options(event_timeout_target PRIVATE -Os -g3 -Wall -Wextra -Werror
    -Wno-unused-parameter -ffunction-sections -fdata-sections -fstack-usage)
target_link_options(event_timeout_target PRIVATE -Wl,--gc-sections
    "-T${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/event_timeout_target.map")
