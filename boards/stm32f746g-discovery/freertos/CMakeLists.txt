# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# STM32F746G-Discovery FreeRTOS Build
# ============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveCommon.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveFreeRTOS.cmake)

ove_setup_project(firmware)

set(STM32CUBE_PATH ${OVE_DL_DIR}/STM32CubeF7)

# Board-specific compile definitions
ove_add_compile_definitions(
    __FPU_PRESENT=1
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    $<$<BOOL:${OVE_NET}>:HAL_ETH_MODULE_ENABLED>
)

# Board-specific compile options
ove_add_compile_options(
    -ffast-math
    -fsingle-precision-constant
)

#############################################################################
# Include directories
#############################################################################
ove_add_include_directories(
    # Board-specific
    ${BOARD_DIR}/inc/lvgl
    ${BOARD_DIR}/inc/stm32f7
    ${BOARD_DIR}/inc/stm32f7/fatfs
    ${OVE_DIR}/drivers/freertos/stm32f7
    # STM32CubeF7
    ${STM32CUBE_PATH}/Drivers/CMSIS/Include
    ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Include
    ${STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx/Include
    ${STM32CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver/Inc
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery
    ${STM32CUBE_PATH}/Drivers/BSP/Components/wm8994
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FreeRTOS/Source/include
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src/drivers
    ${STM32CUBE_PATH}/Utilities/CPU
    # LVGL
    ${OVE_DL_DIR}/lvgl
    ${OVE_DL_DIR}
)

#############################################################################
# Board platform sources
#############################################################################
ove_add_board_sources(
    main.c
    src/FreeRTOS-openocd.c
    src/serial_wrapper.c
)
if(OVE_AUDIO)
    ove_add_board_sources(src/bus_msp_init.c)
endif()
if(OVE_NET)
    ove_add_board_sources(${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_eth.c)
endif()

# STM32F7 HAL/BSP/startup sources
set(STM32F7_SOURCES
    ${STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/system_stm32f7xx.c
    ${BOARD_DIR}/src/stm32f7/syscalls.c
    ${BOARD_DIR}/src/stm32f7/bsp.c
    ${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_init.c
    ${STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/gcc/startup_stm32f746xx.s
)

set(LVGL_PORT_SOURCES
    ${BOARD_DIR}/src/lvgl/lv_port_disp.c
)

# FreeRTOS kernel (from STM32CubeF7 middleware)
ove_add_freertos_kernel(ARM_CM7/r0p1)

# oveRTOS backend sources (from generated config)
ove_add_backend_sources()

# Heap management
if(NOT OVE_ZERO_HEAP)
    ove_add_board_sources(${STM32CUBE_PATH}/Middlewares/Third_Party/FreeRTOS/Source/portable/MemMang/heap_4.c)
else()
    ove_add_board_sources(${OVE_DIR}/backends/freertos/freertos_heap_stubs.c)
endif()

#############################################################################
# STM32 HAL Driver
#############################################################################
file(GLOB HAL_SOURCES
    ${STM32CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver/Src/*.c
)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

#############################################################################
# BSP
#############################################################################
set(BSP_SOURCES
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sd.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sdram.c
    ${STM32CUBE_PATH}/Drivers/BSP/Components/wm8994/wm8994.c
)

#############################################################################
# FatFs
#############################################################################
file(GLOB FATFS_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src/*.c
)
set(FATFS_EXTRA_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src/option/ccsbcs.c
    ${BOARD_DIR}/src/stm32f7/fatfs/sd_diskio.c
)

#############################################################################
# CMSIS DSP Library
#############################################################################
file(GLOB DSP_BASIC_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/BasicMathFunctions/*.c)
file(GLOB DSP_COMMON_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/CommonTables/*.c)
file(GLOB DSP_COMPLEX_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/ComplexMathFunctions/*.c)
file(GLOB DSP_CONTROLLER_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/ControllerFunctions/*.c)
file(GLOB DSP_FAST_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/FastMathFunctions/*.c)
file(GLOB DSP_FILTER_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/FilteringFunctions/*.c)
file(GLOB DSP_MATRIX_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/MatrixFunctions/*.c)
file(GLOB DSP_STATS_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/StatisticsFunctions/*.c)
file(GLOB DSP_SUPPORT_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/SupportFunctions/*.c)
file(GLOB DSP_TRANSFORM_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/TransformFunctions/*.c)
file(GLOB DSP_TRANSFORM_ASM_SOURCES ${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Source/TransformFunctions/*.S)

set(DSP_SOURCES
    ${DSP_BASIC_SOURCES}
    ${DSP_COMMON_SOURCES}
    ${DSP_COMPLEX_SOURCES}
    ${DSP_CONTROLLER_SOURCES}
    ${DSP_FAST_SOURCES}
    ${DSP_FILTER_SOURCES}
    ${DSP_MATRIX_SOURCES}
    ${DSP_STATS_SOURCES}
    ${DSP_SUPPORT_SOURCES}
    ${DSP_TRANSFORM_SOURCES}
    ${DSP_TRANSFORM_ASM_SOURCES}
)

#############################################################################
# LVGL
#############################################################################
ove_build_lvgl()

#############################################################################
# ML inference (TensorFlow Lite Micro)
#############################################################################
if(OVE_INFER)
    set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
    set(OVE_CMSIS5_DIR ${OVE_DIR}/dl/CMSIS_5/CMSIS)
    set(OVE_CMSIS_NN_DIR ${OVE_CMSIS5_DIR}/NN)
    include(${OVE_DIR}/cmake/OveTflm.cmake)
    ove_build_tflm()
endif()

# Networking
if(OVE_NET)
    include(${OVE_DIR}/cmake/OveLwIP.cmake)
    ove_build_lwip()
endif()
if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

#############################################################################
# Build firmware
#############################################################################
set(_OVE_EXTRA_SOURCES
    ${STM32F7_SOURCES}
    ${LVGL_PORT_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${FATFS_SOURCES}
    ${FATFS_EXTRA_SOURCES}
    ${DSP_SOURCES}
    ${STM32CUBE_PATH}/Utilities/CPU/cpu_utils.c
)

ove_link_firmware(STM32F746NGHx_FLASH.ld)

target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE
    $<$<BOOL:${OVE_INFER}>:ove_tflm>
    $<$<BOOL:${OVE_NET}>:ove_lwip>
    $<$<BOOL:${OVE_NET_TLS}>:ove_mbedtls>
    m
)

#############################################################################
# Post-build commands
#############################################################################
add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex ${_OVE_PROJ_NAME}.elf ${_OVE_PROJ_NAME}.hex
    COMMENT "Generating ${_OVE_PROJ_NAME}.hex"
)
add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${_OVE_PROJ_NAME}.elf ${_OVE_PROJ_NAME}.bin
    COMMENT "Generating ${_OVE_PROJ_NAME}.bin"
)
add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJDUMP} -h -S ${_OVE_PROJ_NAME}.elf > ${_OVE_PROJ_NAME}.lst
    COMMENT "Generating ${_OVE_PROJ_NAME}.lst"
)
add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DELF_FILE=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.elf
        -DSIZE_TOOL=${CMAKE_SIZE}
        -DFLASH_SIZE=${OVE_FLASH_SIZE}
        -DRAM_SIZE=${OVE_RAM_SIZE}
        -P ${BOARD_DIR}/cmake/print_size.cmake
    COMMENT "Memory usage:"
)

#############################################################################
# Flash target
#############################################################################
add_custom_target(flash
    COMMAND openocd
        -f board/stm32f7discovery.cfg
        -c "init"
        -c "reset init"
        -c "flash probe 0"
        -c "flash write_image erase ${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.elf 0x0"
        -c "reset run"
        -c shutdown
    DEPENDS ${_OVE_PROJ_NAME}.elf
    COMMENT "Flashing ${_OVE_PROJ_NAME}.elf to target"
)
