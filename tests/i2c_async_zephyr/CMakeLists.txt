# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_i2c_retirement_zephyr)
get_filename_component(OVE_ROOT "../.." ABSOLUTE)
target_sources(app PRIVATE main.c ${OVE_ROOT}/src/ove_i2c.c
    ${OVE_ROOT}/backends/zephyr/zephyr_i2c.c
    ${OVE_ROOT}/backends/zephyr/zephyr_sync.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/zephyr/include ${OVE_ROOT}/backends/common ${OVE_ROOT}/tests)
zephyr_ld_options(-Wl,--wrap=ove_i2c_async_complete -Wl,--wrap=ove_hal_i2c_close)
zephyr_ld_options(-Wl,--wrap=ove_hal_i2c_write_read_async)
option(OVE_I2C_CAPTURE_PARENT "Avoid reinitializing running work in parent comparison" OFF)
if(OVE_I2C_CAPTURE_PARENT)
    target_compile_definitions(app PRIVATE OVE_I2C_CAPTURE_PARENT=1)
endif()
