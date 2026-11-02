# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_i2c_native_boundary)
get_filename_component(OVE_ROOT "../../.." ABSOLUTE)
target_sources(app PRIVATE main.c ${OVE_ROOT}/src/ove_i2c.c
    ${OVE_ROOT}/backends/zephyr/zephyr_i2c.c
    ${OVE_ROOT}/backends/zephyr/zephyr_sync.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/zephyr/include ${OVE_ROOT}/backends/common)
