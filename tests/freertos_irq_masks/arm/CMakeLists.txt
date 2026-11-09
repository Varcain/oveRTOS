# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ove_irq_masks_target C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
option(OVE_MPU_V1 "Use the admitted MPU V1 native port" OFF)
option(OVE_ZERO_HEAP "Disable dynamic kernel allocation" OFF)
option(OVE_SYSCALL_ENFORCE "Enforce native syscall origins" OFF)
option(OVE_ERRATUM "Use the native V1 erratum workaround" OFF)
option(OVE_ALL_CALLERS_PRIVILEGED "Qualify source comparisons without restricted callers" OFF)
option(OVE_IRQ_TARGET_CANARY "Keep an early failure through later controls" OFF)
set(OVE_ALLOW_UNPRIVILEGED_CRITICAL 0 CACHE STRING "Native raw-critical compatibility")
set(OVE_MPU_REGIONS 16 CACHE STRING "Actual native MPU region count")
set(OVE_NATIVE_TICK_BITS 32 CACHE STRING "Actual native tick width")
set(OVE_IRQ_PROVIDER "${OVE_ROOT}/backends/freertos/freertos_irq.c" CACHE FILEPATH "IRQ provider")
get_filename_component(provider "${OVE_IRQ_PROVIDER}" REALPATH)
if(NOT provider STREQUAL "${OVE_ROOT}/backends/freertos/freertos_irq.c" AND NOT OVE_ALL_CALLERS_PRIVILEGED)
    message(FATAL_ERROR "Alternate providers require OVE_ALL_CALLERS_PRIVILEGED")
endif()
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
if(OVE_MPU_V1)
    set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM4_MPU")
endif()
if(NOT EXISTS "${port}/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the pinned native kernel")
endif()
file(READ "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s" startup)
string(FIND "${startup}" "    .long   Default_Handler         /*  1: UART0 TX */" slot)
if(slot LESS 0)
    message(FATAL_ERROR "Expected native IRQ vector is absent")
endif()
string(REPLACE "    .long   Default_Handler         /*  1: UART0 TX */"
    "    .long   UART0TX_IRQHandler      /*  1: UART0 TX */" startup "${startup}")
file(WRITE "${CMAKE_BINARY_DIR}/irq_startup.s" "${startup}")
add_executable(irq_masks_target main.c "${OVE_IRQ_PROVIDER}"
    "${OVE_ROOT}/backends/freertos/freertos_hooks.c"
    "${FREERTOS_PATH}/tasks.c" "${FREERTOS_PATH}/queue.c" "${FREERTOS_PATH}/list.c"
    "${FREERTOS_PATH}/event_groups.c" "${port}/port.c"
    "${CMAKE_BINARY_DIR}/irq_startup.s"
    "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c")
target_compile_definitions(irq_masks_target PRIVATE OVE_FREERTOS_IRQ_PORT_SUPPORTED=1
    OVE_NATIVE_ABORT=0 OVE_NATIVE_TICK_BITS=${OVE_NATIVE_TICK_BITS}
    OVE_ALLOW_UNPRIVILEGED_CRITICAL=${OVE_ALLOW_UNPRIVILEGED_CRITICAL}
    OVE_SYSCALL_ENFORCE=$<BOOL:${OVE_SYSCALL_ENFORCE}> OVE_ERRATUM=$<BOOL:${OVE_ERRATUM}>
    OVE_ALL_CALLERS_PRIVILEGED=$<BOOL:${OVE_ALL_CALLERS_PRIVILEGED}>
    OVE_IRQ_TARGET_CANARY=$<BOOL:${OVE_IRQ_TARGET_CANARY}>)
if(OVE_MPU_V1)
    target_compile_definitions(irq_masks_target PRIVATE OVE_MPU_V1=1 OVE_MPU_REGIONS=${OVE_MPU_REGIONS})
    target_sources(irq_masks_target PRIVATE "${FREERTOS_PATH}/portable/Common/mpu_wrappers.c"
        "${FREERTOS_PATH}/stream_buffer.c")
endif()
if(OVE_ZERO_HEAP)
    target_compile_definitions(irq_masks_target PRIVATE CONFIG_OVE_ZERO_HEAP=1)
    target_sources(irq_masks_target PRIVATE "${OVE_ROOT}/backends/freertos/freertos_heap_stubs.c")
else()
    target_sources(irq_masks_target PRIVATE "${FREERTOS_PATH}/portable/MemMang/heap_4.c")
endif()
target_include_directories(irq_masks_target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${FREERTOS_PATH}/include" "${port}" "${OVE_ROOT}/include"
    "${OVE_ROOT}/backends/freertos/include" "${OVE_ROOT}/backends/common"
    "${OVE_ROOT}/tests")
target_compile_options(irq_masks_target PRIVATE -Os -g3 -Wall -Wextra -Werror
    -Wno-unused-parameter -ffunction-sections -fdata-sections -fstack-usage)
file(READ "${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld" linker)
string(REPLACE "        KEEP(*(.isr_vector))"
    "        KEEP(*(.isr_vector))\n        KEEP(*(.irq_profile))" linker "${linker}")
string(REPLACE "        *(freertos_system_calls)"
    "        __syscalls_flash_start__ = .;\n        *(freertos_system_calls)\n        __syscalls_flash_end__ = .;" linker "${linker}")
file(WRITE "${CMAKE_BINARY_DIR}/irq_masks.ld" "${linker}")
target_link_options(irq_masks_target PRIVATE -Wl,--gc-sections
    "-T${CMAKE_BINARY_DIR}/irq_masks.ld" "-Wl,-Map=${CMAKE_BINARY_DIR}/irq_masks_target.map")
