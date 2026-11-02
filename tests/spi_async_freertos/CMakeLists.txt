# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.14)
project(ove_spi_retirement_freertos C ASM)
option(OVE_ARM "Run the real Cortex-M7 kernel port under QEMU" OFF)
option(OVE_RUST_SPI_TEST "Exercise the production Rust SPI/Embassy path" OFF)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
if(NOT EXISTS "${FREERTOS_PATH}/portable/ThirdParty/GCC/Posix/port.c")
    message(FATAL_ERROR "FREERTOS_PATH must name the manifest-pinned kernel")
endif()
if(OVE_ARM)
    set(port "${FREERTOS_PATH}/portable/GCC/ARM_CM7/r0p1")
    set(port_sources ${port}/port.c
        ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/startup_CMSDK_CM7.s
        ${OVE_ROOT}/boards/qemu-mps2-an500/freertos/system_CMSDK_CM7.c)
else()
    set(port "${FREERTOS_PATH}/portable/ThirdParty/GCC/Posix")
    set(port_sources ${port}/port.c ${port}/utils/wait_for_event.c)
endif()
add_executable(ove_test_spi_freertos main.c ${OVE_ROOT}/src/ove_spi.c
    ${OVE_ROOT}/drivers/freertos/stm32f7/stm32f7_spi.c
    ${OVE_ROOT}/backends/freertos/freertos_sync.c
    ${FREERTOS_PATH}/tasks.c ${FREERTOS_PATH}/queue.c ${FREERTOS_PATH}/list.c
    ${FREERTOS_PATH}/timers.c ${FREERTOS_PATH}/portable/MemMang/heap_4.c
    ${port_sources})
# Real kernel/port headers precede the dependency-only STM32 SDK fixture.
target_include_directories(ove_test_spi_freertos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${FREERTOS_PATH}/include ${port} ${port}/utils
    ${OVE_ROOT}/tests/spi_native/sdk ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/freertos/include ${OVE_ROOT}/backends/common ${OVE_ROOT}/tests)
target_compile_definitions(ove_test_spi_freertos PRIVATE STM32F746xx=1)
if(OVE_RUST_SPI_TEST)
    if(NOT OVE_ARM)
        message(FATAL_ERROR "Rust SPI stack fixture requires the ARM port")
    endif()
    target_compile_definitions(ove_test_spi_freertos PRIVATE OVE_RUST_SPI_TEST=1)
    target_sources(ove_test_spi_freertos PRIVATE ${OVE_ROOT}/src/ove_sync.c)
    set(OVE_DIR "${OVE_ROOT}")
    set(OVE_SPI ON)
    set(OVE_SYNC ON)
    include(${OVE_ROOT}/cmake/OveBindingsCommon.cmake)
    _ove_binding_write_sizes_probe("${CMAKE_BINARY_DIR}/ove_storage_sizes.c"
        "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
    _ove_binding_build_sizes_probe(_rust_spi_sizes ove_test_spi_freertos
        "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
    _ove_binding_extract_sizes("${CMAKE_BINARY_DIR}/ove_storage_sizes.env"
        _rust_spi_sizes "Extracting actual ARM fixture storage sizes")
    add_custom_target(storage_sizes DEPENDS "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
    target_link_libraries(ove_test_spi_freertos PRIVATE "${OVE_RUST_SPI_LIBRARY}")
endif()
if(OVE_ARM)
    target_compile_definitions(ove_test_spi_freertos PRIVATE OVE_ARM=1)
    target_compile_options(ove_test_spi_freertos PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(ove_test_spi_freertos PRIVATE
        -T${OVE_ROOT}/boards/qemu-mps2-an500/freertos/mps2_an500.ld -Wl,--gc-sections)
else()
    target_link_libraries(ove_test_spi_freertos PRIVATE pthread rt)
endif()
target_link_options(ove_test_spi_freertos PRIVATE
    -Wl,--wrap=xTimerPendFunctionCallFromISR -Wl,--wrap=xTimerPendFunctionCall
    -Wl,--wrap=xTimerCreateStatic)
