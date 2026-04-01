# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Path configuration — set by oveRTOS top-level Makefile
# ============================================================================
# OVE_DIR       — ove root (parent of boards/)
# OVE_APP_DIR   — application directory
# OVE_GEN_DIR   — output/generated/
# OVE_DL_DIR    — dl/ (downloaded sources)
# BOARD_DIR         — this directory

if(NOT DEFINED OVE_DIR)
    get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
if(NOT DEFINED BOARD_DIR)
    set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})
endif()
include(${OVE_DIR}/cmake/OveAppResolve.cmake)
if(NOT DEFINED OVE_GEN_DIR)
    set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
endif()
if(NOT DEFINED OVE_DL_DIR)
    message(FATAL_ERROR "OVE_DL_DIR must be set by the build system (e.g. -DOVE_DL_DIR=<workspace>/dl)")
endif()

set(STM32CUBE_PATH ${OVE_DL_DIR}/STM32CubeF7)
set(LVGL_PATH ${OVE_DL_DIR}/lvgl)

# Include generated CMake config
if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
    include(${OVE_GEN_DIR}/ove_config.cmake)
endif()

# Default to Release for -O2 (DSP needs optimization to meet audio deadlines)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Project — include CXX if app language requires it, or if ML inference is enabled
set(PROJ_NAME firmware)
if(OVE_APP_LANG STREQUAL "cpp" OR OVE_INFER)
    project(${PROJ_NAME} C CXX ASM)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    project(${PROJ_NAME} C ASM)
endif()
enable_language(ASM)

#############################################################################
# Toolchain (loaded from generated config or defaults)
#############################################################################
include(${BOARD_DIR}/cmake/arm-none-eabi.cmake OPTIONAL)

#############################################################################
# Compiler definitions
#############################################################################
add_compile_definitions(
    __FPU_PRESENT=1
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    LV_CONF_INCLUDE_SIMPLE
    $<$<BOOL:${OVE_NET}>:HAL_ETH_MODULE_ENABLED>
)

#############################################################################
# Compiler flags
#############################################################################
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall
    -Wno-missing-braces
    -ffunction-sections
    -fdata-sections
    -ffast-math
    -fsingle-precision-constant
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-O2>
)

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -g -x assembler-with-cpp")

#############################################################################
# Application source list (generated from app.yaml by 'ove configure')
#############################################################################
if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
    include(${OVE_GEN_DIR}/app_sources.cmake)
elseif(EXISTS "${OVE_APP_DIR}/CMakeLists.txt")
    message(WARNING "Using legacy app CMakeLists.txt — migrate to app.yaml")
    include(${OVE_APP_DIR}/CMakeLists.txt)
else()
    message(FATAL_ERROR "No app build description found. Run 'ove configure' first.")
endif()

#############################################################################
# Include directories
#############################################################################
include_directories(
    # Generated config headers
    ${OVE_GEN_DIR}
    # Application
    ${APP_INCLUDE_DIRS}
    # oveRTOS framework
    ${OVE_DIR}/include
    ${OVE_DIR}/bindings/cpp
    ${OVE_DIR}/backends/freertos/include
    ${OVE_DIR}/backends/common
    # Board-specific
    ${BOARD_DIR}/inc
    ${BOARD_DIR}/inc/lvgl
    ${BOARD_DIR}
    ${BOARD_DIR}/inc/stm32f7
    ${BOARD_DIR}/inc/stm32f7/fatfs
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
    ${LVGL_PATH}
    ${OVE_DL_DIR}
)

#############################################################################
# Source files — Board platform code
#############################################################################
set(BOARD_SOURCES
    ${BOARD_DIR}/main.c
    ${BOARD_DIR}/src/FreeRTOS-openocd.c
    ${BOARD_DIR}/src/serial_wrapper.c
)
if(NOT OVE_ZERO_HEAP)
    list(APPEND BOARD_SOURCES ${BOARD_DIR}/src/heap_4.c)
else()
    list(APPEND BOARD_SOURCES ${OVE_DIR}/backends/freertos/freertos_heap_stubs.c)
endif()
if(OVE_AUDIO)
    list(APPEND BOARD_SOURCES
        ${BOARD_DIR}/src/audio_codec_da.c
        ${BOARD_DIR}/src/i2s_da.c
    )
endif()
if(OVE_NET)
    list(APPEND BOARD_SOURCES ${BOARD_DIR}/src/eth_stm32f7.c)
endif()

set(STM32F7_SOURCES
    ${BOARD_DIR}/src/stm32f7/system_stm32f7xx.c
    ${BOARD_DIR}/src/stm32f7/syscalls.c
    ${BOARD_DIR}/src/stm32f7/bsp.c
    ${BOARD_DIR}/src/stm32f7/startup_stm32f746xx.s
)
if(OVE_AUDIO)
    list(APPEND STM32F7_SOURCES ${BOARD_DIR}/src/stm32f7/i2s_stm32f7.c)
endif()

set(LVGL_PORT_SOURCES
    ${BOARD_DIR}/src/lvgl/lv_port_disp.c
)

#############################################################################
# Source files — oveRTOS framework (dispatchers + backend)
#############################################################################
# Use generated source list if available, otherwise include all
if(DEFINED OVE_BACKEND_SOURCES)
    set(OVE_SOURCES ${OVE_DIR}/src/ove_app.c ${OVE_BACKEND_SOURCES})
else()
    set(OVE_SOURCES
        ${OVE_DIR}/src/ove_app.c
        ${OVE_DIR}/backends/freertos/freertos_thread.c
        ${OVE_DIR}/backends/freertos/freertos_sync.c
        ${OVE_DIR}/backends/freertos/freertos_audio.c
        ${OVE_DIR}/backends/freertos/freertos_fs.c
        ${OVE_DIR}/backends/freertos/freertos_console.c
        ${OVE_DIR}/backends/freertos/freertos_time.c
        ${OVE_DIR}/backends/freertos/freertos_lvgl.c
        ${OVE_DIR}/backends/freertos/freertos_queue.c
        ${OVE_DIR}/backends/freertos/freertos_timer.c
        ${OVE_DIR}/backends/freertos/stm32f7_bsp.c
        ${OVE_DIR}/backends/freertos/freertos_eventgroup.c
        ${OVE_DIR}/backends/freertos/freertos_workqueue.c
        ${OVE_DIR}/backends/freertos/freertos_stream.c
        ${OVE_DIR}/backends/freertos/freertos_watchdog.c
        ${OVE_DIR}/backends/freertos/freertos_nvs.c
        ${OVE_DIR}/backends/freertos/freertos_shell.c
    )
endif()

#############################################################################
# Source files — STM32 HAL Driver
#############################################################################
file(GLOB HAL_SOURCES
    ${STM32CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver/Src/*.c
)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

#############################################################################
# Source files — BSP
#############################################################################
set(BSP_SOURCES
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sd.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_lcd.c
    ${STM32CUBE_PATH}/Drivers/BSP/STM32746G-Discovery/stm32746g_discovery_sdram.c
    ${STM32CUBE_PATH}/Drivers/BSP/Components/wm8994/wm8994.c
)

#############################################################################
# Source files — FreeRTOS
#############################################################################
file(GLOB FREERTOS_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FreeRTOS/Source/*.c
)
set(FREERTOS_PORT_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM7/r0p1/port.c
)

#############################################################################
# Source files — FatFs
#############################################################################
file(GLOB FATFS_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src/*.c
)
set(FATFS_EXTRA_SOURCES
    ${STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src/option/ccsbcs.c
    ${BOARD_DIR}/src/stm32f7/fatfs/sd_diskio.c
)

#############################################################################
# Source files — CPU Utilities
#############################################################################
set(UTIL_SOURCES
    ${STM32CUBE_PATH}/Utilities/CPU/cpu_utils.c
)

#############################################################################
# Source files — CMSIS DSP Library
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
# Source files — LVGL
#############################################################################
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_PATH}/src/*.c)
add_library(lvgl ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC ${LVGL_PATH})

#############################################################################
# ML inference (TensorFlow Lite Micro)
#############################################################################
if(OVE_INFER)
    set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
    # CMSIS_5 is in the global dl/ dir (not workspace-local)
    set(OVE_CMSIS5_DIR ${OVE_DIR}/dl/CMSIS_5/CMSIS)
    set(OVE_CMSIS_NN_DIR ${OVE_CMSIS5_DIR}/NN)
    include(${OVE_DIR}/cmake/OveTflm.cmake)
    ove_build_tflm()
endif()
if(OVE_NET)
    include(${OVE_DIR}/cmake/OveLwIP.cmake)
    ove_build_lwip()
endif()
if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

#############################################################################
# Application language support
#############################################################################
include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)

#############################################################################
# All sources combined (app sources handled by ove_apply_app_language)
#############################################################################
set(ALL_SOURCES
    ${BOARD_SOURCES}
    ${OVE_SOURCES}
    ${STM32F7_SOURCES}
    ${LVGL_PORT_SOURCES}
    ${HAL_SOURCES}
    ${BSP_SOURCES}
    ${FREERTOS_SOURCES}
    ${FREERTOS_PORT_SOURCES}
    ${FATFS_SOURCES}
    ${FATFS_EXTRA_SOURCES}
    ${DSP_SOURCES}
    ${UTIL_SOURCES}
)

#############################################################################
# Executable
#############################################################################
add_executable(${PROJ_NAME}.elf ${ALL_SOURCES})
ove_apply_app_language(${PROJ_NAME}.elf)

# Linker script
set(LINKER_SCRIPT ${BOARD_DIR}/STM32F746NGHx_FLASH.ld)

# Linker flags
target_link_options(${PROJ_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_BINARY_DIR}/${PROJ_NAME}.map
    -Wl,--undefined=uxTopUsedPriority
)

# Link libraries
target_link_libraries(${PROJ_NAME}.elf PRIVATE
    lvgl
    $<$<BOOL:${OVE_INFER}>:ove_tflm>
    $<$<BOOL:${OVE_NET}>:ove_lwip>
    $<$<BOOL:${OVE_NET_TLS}>:ove_mbedtls>
    m
)

#############################################################################
# Post-build commands
#############################################################################
add_custom_command(TARGET ${PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex ${PROJ_NAME}.elf ${PROJ_NAME}.hex
    COMMENT "Generating ${PROJ_NAME}.hex"
)
add_custom_command(TARGET ${PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary ${PROJ_NAME}.elf ${PROJ_NAME}.bin
    COMMENT "Generating ${PROJ_NAME}.bin"
)
add_custom_command(TARGET ${PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJDUMP} -h -S ${PROJ_NAME}.elf > ${PROJ_NAME}.lst
    COMMENT "Generating ${PROJ_NAME}.lst"
)
add_custom_command(TARGET ${PROJ_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DELF_FILE=${CMAKE_BINARY_DIR}/${PROJ_NAME}.elf
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
        -c "flash write_image erase ${CMAKE_BINARY_DIR}/${PROJ_NAME}.elf 0x0"
        -c "reset run"
        -c shutdown
    DEPENDS ${PROJ_NAME}.elf
    COMMENT "Flashing ${PROJ_NAME}.elf to target"
)

#############################################################################
# Print configuration
#############################################################################
message(STATUS "oveRTOS FreeRTOS build")
message(STATUS "  OVE_DIR: ${OVE_DIR}")
message(STATUS "  APP_DIR: ${OVE_APP_DIR}")
message(STATUS "  STM32Cube: ${STM32CUBE_PATH}")
message(STATUS "  LVGL: ${LVGL_PATH}")
