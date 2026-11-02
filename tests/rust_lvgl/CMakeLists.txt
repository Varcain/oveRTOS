# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)
project(ove_rust_lvgl C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(OVE_ROOT ../.. ABSOLUTE)
option(OVE_ZERO_HEAP "Use the zero-heap application configuration" OFF)
option(OVE_SANITIZE "Instrument native LVGL and binding helpers" OFF)
option(OVE_CLIB "Use native heap allocations for suballocation sanitizers" OFF)
set(OVE_DRAW_ALIGNMENT 4 CACHE STRING "Native draw-buffer alignment exercised by the fixture")
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
file(READ ${OVE_ROOT}/tests/ove_config.h _config)
if(OVE_ZERO_HEAP)
    string(REPLACE "#endif /* OVE_CONFIG_H */"
        "#define CONFIG_OVE_ZERO_HEAP 1\n\n#endif /* OVE_CONFIG_H */" _config "${_config}")
endif()
file(WRITE ${CMAKE_BINARY_DIR}/generated/ove_config.h "${_config}")
set(_lv_conf "#include \"${OVE_ROOT}/tests/host/lv_conf.h\"\n")
string(APPEND _lv_conf "#undef LV_DRAW_BUF_ALIGN\n#define LV_DRAW_BUF_ALIGN ${OVE_DRAW_ALIGNMENT}\n")
if(OVE_CLIB)
    string(APPEND _lv_conf "#undef LV_USE_STDLIB_MALLOC\n#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB\n")
endif()
file(WRITE ${CMAKE_BINARY_DIR}/generated/lv_conf.h "${_lv_conf}")
include_directories(${CMAKE_BINARY_DIR}/generated ${OVE_ROOT}/include ${OVE_ROOT}/tests ${OVE_ROOT}/dl)
if(OVE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
endif()
file(GLOB_RECURSE LVGL_SOURCES ${OVE_ROOT}/dl/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_compile_definitions(lvgl PUBLIC LV_CONF_PATH="${CMAKE_BINARY_DIR}/generated/lv_conf.h")
set_target_properties(lvgl PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lvgl/lib)
add_library(ove_lvgl_text_test STATIC
    ${OVE_ROOT}/src/ove_lvgl_text.c ${OVE_ROOT}/src/ove_lvgl_canvas.c support.c canvas_support.c)
target_link_libraries(ove_lvgl_text_test PUBLIC lvgl)
