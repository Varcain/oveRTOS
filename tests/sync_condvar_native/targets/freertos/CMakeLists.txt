# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_condvar_freertos C ASM)
get_filename_component(OVE_ROOT "../../../.." ABSOLUTE)
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
if(NOT EXISTS "${port}/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the pinned kernel")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_executable(condvar_target ../native.c
    ${OVE_ROOT}/backends/freertos/freertos_sync.c
    ${FREERTOS_PATH}/tasks.c ${FREERTOS_PATH}/queue.c ${FREERTOS_PATH}/list.c
    ${port}/port.c
    ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s
    ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c)
target_include_directories(condvar_target PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${FREERTOS_PATH}/include ${port} ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/freertos/include ${OVE_ROOT}/backends/freertos
    ${OVE_ROOT}/backends/common ${OVE_ROOT}/tests)
target_compile_options(condvar_target PRIVATE -Os -g3 -ffunction-sections -fdata-sections
    -fstack-usage)
target_link_options(condvar_target PRIVATE
    -T${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld
    -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/condvar_target.map)

option(OVE_CONDVAR_TARGET_CANARY "Run harmless failure calibration" OFF)
if(OVE_CONDVAR_TARGET_CANARY)
    target_compile_definitions(condvar_target PRIVATE OVE_CONDVAR_TARGET_CANARY=1)
endif()
