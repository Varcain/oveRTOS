# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_condvar_zephyr)
get_filename_component(OVE_ROOT "../../../.." ABSOLUTE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
target_sources(app PRIVATE ../native.c ${OVE_ROOT}/backends/zephyr/zephyr_sync.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/zephyr/include ${OVE_ROOT}/backends/common ${OVE_ROOT}/tests)
target_compile_options(app PRIVATE -fstack-usage)

option(OVE_CONDVAR_TARGET_CANARY "Run harmless failure calibration" OFF)
if(OVE_CONDVAR_TARGET_CANARY)
    target_compile_definitions(app PRIVATE OVE_CONDVAR_TARGET_CANARY=1)
endif()
