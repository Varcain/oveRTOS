# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_spi_retirement_zephyr)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
target_sources(app PRIVATE main.c ${OVE_ROOT}/src/ove_spi.c
    ${OVE_ROOT}/backends/zephyr/zephyr_spi.c
    ${OVE_ROOT}/backends/zephyr/zephyr_sync.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/zephyr/include ${OVE_ROOT}/backends/common)
zephyr_ld_options(-Wl,--wrap=ove_spi_async_complete -Wl,--wrap=ove_hal_spi_close)
zephyr_ld_options(-Wl,--wrap=ove_hal_spi_transfer_async)
option(OVE_SPI_CAPTURE_PARENT "Avoid reinitializing running work in parent comparison" OFF)
if(OVE_SPI_CAPTURE_PARENT)
    target_compile_definitions(app PRIVATE OVE_SPI_CAPTURE_PARENT=1)
endif()
