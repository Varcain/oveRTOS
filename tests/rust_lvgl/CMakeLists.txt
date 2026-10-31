# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_rust_lvgl C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT ../.. ABSOLUTE)
include_directories(${OVE_ROOT}/include ${OVE_ROOT}/tests ${OVE_ROOT}/dl)
file(GLOB_RECURSE LVGL_SOURCES ${OVE_ROOT}/dl/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_compile_definitions(lvgl PUBLIC LV_CONF_PATH="${OVE_ROOT}/tests/host/lv_conf.h")
set_target_properties(lvgl PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lvgl/lib)
add_library(ove_lvgl_text_test STATIC
    ${OVE_ROOT}/src/ove_lvgl_text.c support.c)
target_link_libraries(ove_lvgl_text_test PUBLIC lvgl)
