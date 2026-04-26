# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Renode STM32F746 test target — heap mode.  Sibling of the zero-heap
# CMakeLists under ../renode-stm32f746-freertos-zeroheap/; differences
# are limited to the target name, the ELF name, the CONFIG_OVE_ZERO_HEAP
# gate being off, and the FreeRTOS kernel sourcing heap_4.c instead of
# freertos_heap_stubs.c.

cmake_minimum_required(VERSION 3.14)

# OVE_HW: see zero-heap sibling for rationale.  Switches both the
# toolchain shim (rdimon → nosys) and the linked stdio backend.
if(OVE_HW)
    set(CMAKE_TOOLCHAIN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/stm32f746g-discovery/freertos/cmake/arm-none-eabi.cmake
        CACHE FILEPATH "ARM toolchain file")
else()
    set(CMAKE_TOOLCHAIN_FILE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/qemu-mps2-an500/cmake/arm-none-eabi.cmake
        CACHE FILEPATH "ARM toolchain file")
endif()

project(ove_test_renode_stm32f746_freertos C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function
                    -ffunction-sections -fdata-sections)

include(FetchContent)

FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG        V11.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(freertos_kernel)
if(NOT freertos_kernel_POPULATED)
    FetchContent_Populate(freertos_kernel)
endif()

FetchContent_Declare(
    cmocka
    GIT_REPOSITORY https://gitlab.com/cmocka/cmocka.git
    GIT_TAG        cmocka-1.1.7
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(cmocka)
if(NOT cmocka_POPULATED)
    FetchContent_Populate(cmocka)
endif()
add_library(cmocka-static STATIC ${cmocka_SOURCE_DIR}/src/cmocka.c)
target_include_directories(cmocka-static PUBLIC ${cmocka_SOURCE_DIR}/include)
target_include_directories(cmocka-static PRIVATE ${cmocka_SOURCE_DIR}/src)
target_compile_definitions(cmocka-static PRIVATE
    HAVE_STRINGS_H HAVE_INTTYPES_H HAVE_MALLOC_H
    HAVE_SIGNAL_H
    HAVE_SETJMP_H HAVE_STDARG_H HAVE_STDDEF_H HAVE_STDINT_H
    HAVE_STDIO_H HAVE_STDLIB_H HAVE_STRING_H
    HAVE_CALLOC HAVE_EXIT HAVE_FREE HAVE_LONGJMP HAVE_MALLOC
    HAVE_MEMCPY HAVE_MEMSET HAVE_SETJMP HAVE_STRCMP HAVE_SPRINTF
    HAVE_VSNPRINTF HAVE_FPRINTF HAVE_PRINTF
)
target_compile_options(cmocka-static PRIVATE
    -Wno-unused-parameter -Wno-implicit-function-declaration -Wno-int-conversion)

set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(OVE_INCLUDE ${OVE_ROOT}/include)
set(OVE_SRC ${OVE_ROOT}/src)
set(FREERTOS_BACKEND ${OVE_ROOT}/backends/freertos)
set(TESTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STUB_DIR ${TESTS_ROOT}/backends/stub)
set(SUITE_DIR ${TESTS_ROOT}/suites)
include(${TESTS_ROOT}/cmake/OveTest.cmake)
set(BOARD_DIR ${OVE_ROOT}/boards/stm32f746g-discovery)
set(FREERTOS_SRC ${freertos_kernel_SOURCE_DIR})
set(FREERTOS_PORT ${FREERTOS_SRC}/portable/GCC/ARM_CM7/r0p1)

file(GLOB _CUBE_F7_CANDIDATES "${OVE_ROOT}/dl/STM32CubeF7-*")
if(NOT _CUBE_F7_CANDIDATES)
    message(FATAL_ERROR
        "STM32CubeF7 not found under ${OVE_ROOT}/dl/. "
        "Run 'make download' or any 'make stm32f746g-discovery.freertos.<app>' first.")
endif()
list(GET _CUBE_F7_CANDIDATES 0 STM32CUBE_PATH)
message(STATUS "STM32CubeF7: ${STM32CUBE_PATH}")

set(STM32_HAL_DIR  ${STM32CUBE_PATH}/Drivers/STM32F7xx_HAL_Driver)
set(STM32_CMSIS_DIR ${STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32F7xx)
set(CMSIS_CORE_DIR ${STM32CUBE_PATH}/Drivers/CMSIS/Include)

set(STARTUP_SOURCES
    ${STM32_CMSIS_DIR}/Source/Templates/gcc/startup_stm32f746xx.s
    ${STM32_CMSIS_DIR}/Source/Templates/system_stm32f7xx.c
)

file(GLOB HAL_SOURCES CONFIGURE_DEPENDS "${STM32_HAL_DIR}/Src/*.c")
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

# --- FreeRTOS kernel sources — heap_4.c for real heap mode. ---
set(FREERTOS_KERNEL_SOURCES
    ${FREERTOS_SRC}/tasks.c
    ${FREERTOS_SRC}/queue.c
    ${FREERTOS_SRC}/list.c
    ${FREERTOS_SRC}/timers.c
    ${FREERTOS_SRC}/event_groups.c
    ${FREERTOS_SRC}/stream_buffer.c
    ${FREERTOS_SRC}/portable/MemMang/heap_4.c
    ${FREERTOS_PORT}/port.c
)

set(FREERTOS_BACKEND_SOURCES
    ${FREERTOS_BACKEND}/freertos_thread.c
    ${FREERTOS_BACKEND}/freertos_sync.c
    ${FREERTOS_BACKEND}/freertos_queue.c
    ${FREERTOS_BACKEND}/freertos_timer.c
    ${FREERTOS_BACKEND}/freertos_eventgroup.c
    ${FREERTOS_BACKEND}/freertos_stream.c
    ${FREERTOS_BACKEND}/freertos_workqueue.c
    ${FREERTOS_BACKEND}/freertos_watchdog.c
    ${FREERTOS_BACKEND}/freertos_gpio.c
    ${FREERTOS_BACKEND}/freertos_shell.c
    # Networking — see zero-heap CMakeLists for rationale.
    ${FREERTOS_BACKEND}/freertos_net.c
    ${FREERTOS_BACKEND}/lwip_sys_arch.c
    # freertos_time.c is intentionally NOT linked — its ove_time_delay_us
    # spins on DWT->CYCCNT which Renode 1.16 doesn't advance.  stub_time
    # has an API-equivalent implementation built on FreeRTOS ticks.
    # freertos_board / nvs / lvgl deferred — see zero-heap CMakeLists.
)

set(CORE_SOURCES
    ${OVE_SRC}/ove_board.c
    ${OVE_SRC}/ove_gpio.c
    ${OVE_SRC}/ove_led.c
    ${OVE_SRC}/ove_app.c
    ${OVE_SRC}/ove_i2c.c
    ${OVE_SRC}/ove_spi.c
    ${OVE_SRC}/ove_uart.c
)

set(STM32F7_DRIVER_SOURCES
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_i2c.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_spi.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_uart.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_eth.c
)

set(STUB_SOURCES
    ${OVE_ROOT}/backends/common/ove_audio_graph.c
    ${OVE_ROOT}/backends/common/ove_audio_nodes.c
    ${STUB_DIR}/stub_bsp.c
    ${STUB_DIR}/stub_lvgl.c
    # stub_gpio removed — freertos_gpio.c + HAL_GPIO_* now drive the
    # STM32_GPIOPort peripherals Renode models.
    ${STUB_DIR}/stub_board.c
    ${STUB_DIR}/stub_time.c
    ${STUB_DIR}/stub_console.c
    # stub_shell removed — freertos_shell.c backs ove_shell_* directly.
    ${STUB_DIR}/stub_nvs.c
)

ove_test_common_suite_sources(SUITE_SOURCES ${SUITE_DIR})

# --- lwIP static library (mirror of zero-heap variant) ---
file(GLOB _CUBE_LWIP_CANDIDATES "${OVE_ROOT}/dl/lwip-*")
if(NOT _CUBE_LWIP_CANDIDATES)
    message(FATAL_ERROR
        "lwIP not found under ${OVE_ROOT}/dl/. Run 'make download'.")
endif()
list(GET _CUBE_LWIP_CANDIDATES 0 LWIP_PATH)
message(STATUS "lwIP: ${LWIP_PATH}")
file(GLOB LWIP_CORE_SRC CONFIGURE_DEPENDS
    "${LWIP_PATH}/src/core/*.c"
    "${LWIP_PATH}/src/core/ipv4/*.c"
    "${LWIP_PATH}/src/api/*.c"
    "${LWIP_PATH}/src/netif/ethernet.c"
)
add_library(ove_lwip STATIC
    ${LWIP_CORE_SRC}
    ${FREERTOS_BACKEND}/lwip_sys_arch.c
)
target_include_directories(ove_lwip PUBLIC
    ${LWIP_PATH}/src/include
    ${FREERTOS_BACKEND}/lwip_port
    ${BOARD_DIR}/freertos/src
)
target_include_directories(ove_lwip PRIVATE
    ${FREERTOS_SRC}/include
    ${FREERTOS_PORT}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OVE_INCLUDE}
)
target_compile_options(ove_lwip PRIVATE -w)
set_target_properties(ove_lwip PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
list(REMOVE_ITEM FREERTOS_BACKEND_SOURCES ${FREERTOS_BACKEND}/lwip_sys_arch.c)

# stdio backend selection — see zero-heap sibling.
if(OVE_HW)
    set(STDIO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/usart1_io.c)
else()
    set(STDIO_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/semihosting_io.c)
endif()

add_executable(ove_test_renode_stm32f746_freertos
    ${CMAKE_CURRENT_SOURCE_DIR}/sim_main.c
    ${STDIO_SOURCE}
    ${STARTUP_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_KERNEL_SOURCES}
    ${FREERTOS_BACKEND_SOURCES}
    ${CORE_SOURCES}
    ${STM32F7_DRIVER_SOURCES}
    ${STUB_SOURCES}
    ${SUITE_SOURCES}
)

target_compile_definitions(ove_test_renode_stm32f746_freertos PRIVATE
    OVE_RENODE_STM32F746
    # OVE_HW propagated to C code so sim_main.c skips the post-summary
    # semihosting_exit() trap on real silicon.
    $<$<BOOL:${OVE_HW}>:OVE_HW>
    OVE_QEMU_ARM
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    # Shared stm32f7xx_hal_conf.h leaves SPI / TIM disabled — re-enable
    # them at the test target level so the storage_t structs and HAL
    # APIs resolve without modifying the production conf.
    HAL_SPI_MODULE_ENABLED
    HAL_TIM_MODULE_ENABLED
    HAL_ETH_MODULE_ENABLED
)

target_include_directories(ove_test_renode_stm32f746_freertos PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FREERTOS_SRC}/include
    ${FREERTOS_PORT}
    ${OVE_INCLUDE}
    ${FREERTOS_BACKEND}/include
    ${OVE_ROOT}/backends/common
    ${TESTS_ROOT}
    ${STM32_HAL_DIR}/Inc
    ${STM32_CMSIS_DIR}/Include
    ${CMSIS_CORE_DIR}
    ${OVE_ROOT}/drivers/freertos/stm32f7
    ${BOARD_DIR}/freertos/inc
    ${LWIP_PATH}/src/include
    ${FREERTOS_BACKEND}/lwip_port
    ${BOARD_DIR}/freertos/src
)

target_link_libraries(ove_test_renode_stm32f746_freertos PRIVATE
    cmocka-static
    ove_lwip
    m
)

set(LINKER_SCRIPT ${BOARD_DIR}/freertos/STM32F746NGHx_FLASH.ld)
target_link_options(ove_test_renode_stm32f746_freertos PRIVATE
    -T${LINKER_SCRIPT}
    -Wl,--gc-sections
)

set_target_properties(ove_test_renode_stm32f746_freertos PROPERTIES
    SUFFIX ".elf"
)

set_source_files_properties(${FREERTOS_KERNEL_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare"
)
set_source_files_properties(${HAL_SOURCES} PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits -Wno-stringop-overflow"
)
set_source_files_properties(${STM32_CMSIS_DIR}/Source/Templates/system_stm32f7xx.c PROPERTIES
    COMPILE_FLAGS "-Wno-unused-parameter"
)
