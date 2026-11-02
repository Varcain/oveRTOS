# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_i2c_stm32_counts C ASM)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
set(hal "${STM32_CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver")
if(NOT EXISTS "${hal}/Inc/stm32f7xx_hal_i2c.h")
    message(FATAL_ERROR "STM32_CUBE_PATH must name the manifest-pinned SDK")
endif()
add_library(i2c_front OBJECT ${OVE_ROOT}/src/ove_i2c.c)
target_compile_definitions(i2c_front PRIVATE ove_mutex_lock=i2c_test_lock)
add_executable(ove_test_i2c_stm32 main.c hal.c $<TARGET_OBJECTS:i2c_front>
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_i2c.c
    ${OVE_ROOT}/backends/freertos/freertos_sync.c
    ${hal}/Src/stm32f7xx_hal_cortex.c
    ${FREERTOS_PATH}/tasks.c ${FREERTOS_PATH}/queue.c ${FREERTOS_PATH}/list.c
    ${FREERTOS_PATH}/timers.c ${FREERTOS_PATH}/portable/MemMang/heap_4.c ${port}/port.c
    ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s
    ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c)
foreach(target ove_test_i2c_stm32 i2c_front)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        ${FREERTOS_PATH}/include ${port} ${OVE_ROOT}/include
        ${OVE_ROOT}/backends/freertos/include ${OVE_ROOT}/backends/common
        ${OVE_ROOT}/drivers/freertos/stm32f7 ${hal}/Inc
        ${STM32_CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx/Include
        ${STM32_CUBE_PATH}/Drivers/CMSIS/Include ${OVE_ROOT}/tests)
    target_compile_definitions(${target} PRIVATE STM32F746xx=1 OVE_ARM=1)
    target_compile_options(${target} PRIVATE -Os -ffunction-sections -fdata-sections)
endforeach()
target_link_options(ove_test_i2c_stm32 PRIVATE -Wl,--gc-sections
    -T${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld)
