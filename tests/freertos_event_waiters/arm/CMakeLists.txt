# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_event_waiters_target C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
option(OVE_SYSCALL_ENFORCE "Enforce native syscall section origins" OFF)
option(OVE_ERRATUM "Retain native Cortex-M7 erratum workaround" OFF)
option(OVE_NATIVE_TRACE "Exercise actual privileged trace marker calls" OFF)
option(OVE_PRIVILEGED_PRODUCER_ONLY "Keep source-comparison producers privileged" OFF)
option(OVE_MPU_V1 "Exercise restricted native caller mode" OFF)
option(OVE_ZERO_HEAP "Use caller-owned objects only" OFF)
option(OVE_NATIVE_ABORT "Enable native abort compatibility controls" OFF)
option(OVE_EVENT_WAITERS_TARGET_CANARY "Retain an early observation failure" OFF)
set(OVE_MPU_REGIONS 16 CACHE STRING "Native QEMU MPU region count")
set(OVE_NATIVE_TICK_BITS 32 CACHE STRING "Native supported tick width")
if(NOT OVE_NATIVE_TICK_BITS MATCHES "^(16|32)$")
    message(FATAL_ERROR "This ARM target supports only 16/32-bit native ticks")
endif()
set(OVE_EVENT_PROVIDER "${OVE_ROOT}/backends/freertos/freertos_sync.c" CACHE FILEPATH "Actual Event provider")
set(OVE_BACKEND_INCLUDE "${OVE_ROOT}/backends/freertos/include" CACHE PATH "Native Event storage headers")
get_filename_component(provider_path "${OVE_EVENT_PROVIDER}" REALPATH)
get_filename_component(backend_path "${OVE_BACKEND_INCLUDE}" REALPATH)
if((NOT provider_path STREQUAL "${OVE_ROOT}/backends/freertos/freertos_sync.c"
    OR NOT backend_path STREQUAL "${OVE_ROOT}/backends/freertos/include")
   AND NOT OVE_PRIVILEGED_PRODUCER_ONLY)
    message(FATAL_ERROR "Alternate native providers require OVE_PRIVILEGED_PRODUCER_ONLY")
endif()
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
if(OVE_MPU_V1)
    set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM4_MPU")
endif()
if(NOT EXISTS "${port}/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the pinned native kernel")
endif()
file(READ "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s" startup)
string(REPLACE "    .long   Default_Handler         /*  1: UART0 TX */"
    "    .long   UART0TX_IRQHandler      /*  1: UART0 TX */" startup "${startup}")
file(WRITE "${CMAKE_BINARY_DIR}/event_waiters_startup.s" "${startup}")
add_executable(event_waiters_target main.c "${OVE_EVENT_PROVIDER}"
    "${OVE_ROOT}/backends/freertos/freertos_eventgroup.c"
    "${OVE_ROOT}/src/ove_sync.c" "${OVE_ROOT}/src/ove_eventgroup.c"
    "${OVE_ROOT}/backends/freertos/freertos_hooks.c"
    "${FREERTOS_PATH}/tasks.c" "${FREERTOS_PATH}/queue.c" "${FREERTOS_PATH}/list.c"
    "${FREERTOS_PATH}/timers.c" "${FREERTOS_PATH}/event_groups.c" "${port}/port.c"
    "${CMAKE_BINARY_DIR}/event_waiters_startup.s"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c")
target_compile_definitions(event_waiters_target PRIVATE OVE_NATIVE_ABORT=$<BOOL:${OVE_NATIVE_ABORT}>
    OVE_PRIVILEGED_PRODUCER_ONLY=$<BOOL:${OVE_PRIVILEGED_PRODUCER_ONLY}>
    OVE_NATIVE_TRACE=$<BOOL:${OVE_NATIVE_TRACE}>
    OVE_SYSCALL_ENFORCE=$<BOOL:${OVE_SYSCALL_ENFORCE}> OVE_ERRATUM=$<BOOL:${OVE_ERRATUM}>
    OVE_NATIVE_TICK_BITS=${OVE_NATIVE_TICK_BITS}
    OVE_EVENT_WAITERS_TARGET_CANARY=$<BOOL:${OVE_EVENT_WAITERS_TARGET_CANARY}>)
if(OVE_NATIVE_TRACE)
    target_include_directories(event_waiters_target PRIVATE "${OVE_ROOT}/sim/include")
    target_sources(event_waiters_target PRIVATE
        "${OVE_ROOT}/backends/freertos/freertos_trace.c"
        "${OVE_ROOT}/backends/common/ove_trace.c" "${OVE_ROOT}/backends/common/ove_trace_ring.c")
endif()
if(OVE_MPU_V1)
    target_compile_definitions(event_waiters_target PRIVATE OVE_MPU_V1=1 OVE_MPU_REGIONS=${OVE_MPU_REGIONS})
    target_sources(event_waiters_target PRIVATE "${FREERTOS_PATH}/portable/Common/mpu_wrappers.c"
        "${FREERTOS_PATH}/stream_buffer.c")
endif()
if(OVE_ZERO_HEAP)
    target_compile_definitions(event_waiters_target PRIVATE CONFIG_OVE_ZERO_HEAP=1)
    target_sources(event_waiters_target PRIVATE "${OVE_ROOT}/backends/freertos/freertos_heap_stubs.c")
else()
    target_sources(event_waiters_target PRIVATE "${FREERTOS_PATH}/portable/MemMang/heap_4.c")
endif()
target_include_directories(event_waiters_target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FREERTOS_PATH}/include" "${port}" "${OVE_ROOT}/include"
    "${OVE_BACKEND_INCLUDE}" "${OVE_ROOT}/backends/freertos/include"
    "${OVE_ROOT}/backends/common" "${OVE_ROOT}/tests")
target_compile_options(event_waiters_target PRIVATE -Os -g3 -Wall -Wextra -Werror
    -Wno-unused-parameter -ffunction-sections -fdata-sections -fstack-usage)
file(READ "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld" linker)
string(REPLACE "        *(freertos_system_calls)"
    "        __syscalls_flash_start__ = .;\n        *(freertos_system_calls)\n        __syscalls_flash_end__ = .;" linker "${linker}")
file(WRITE "${CMAKE_BINARY_DIR}/event_waiters.ld" "${linker}")
if(OVE_MPU_V1)
    target_link_options(event_waiters_target PRIVATE -Wl,--wrap=MPU_ulTaskGenericNotifyTake)
else()
    target_link_options(event_waiters_target PRIVATE -Wl,--wrap=ulTaskGenericNotifyTake)
endif()
target_link_options(event_waiters_target PRIVATE -Wl,--gc-sections
    "-T${CMAKE_BINARY_DIR}/event_waiters.ld"
    "-Wl,-Map=${CMAKE_BINARY_DIR}/event_waiters_target.map")
