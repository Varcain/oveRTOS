# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# oveRTOS Zephyr board CMakeLists.txt
# Paths set by oveRTOS top-level Makefile or defaults.

cmake_minimum_required(VERSION 3.20.0)

# Path defaults
if(NOT DEFINED OVE_DIR)
    get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
include(${OVE_DIR}/cmake/OveAppResolve.cmake)
if(NOT DEFINED OVE_GEN_DIR)
    set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
endif()
set(OVE_BOARD_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Include generated CMake config (sets OVE_RTOS, OVE_APP_LANG, etc.)
if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
    include(${OVE_GEN_DIR}/ove_config.cmake)
endif()

# Add out-of-tree WM8994 driver
list(APPEND ZEPHYR_EXTRA_MODULES ${OVE_BOARD_DIR}/drivers/wm8994)

# Add app-level DTS include path
list(APPEND DTS_ROOT ${OVE_BOARD_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

target_sources(app PRIVATE
    ${OVE_BOARD_DIR}/main.c

    # oveRTOS framework + backend (only enabled modules, from ove_config.cmake)
    ${OVE_BACKEND_SOURCES}
)

# ML inference (TensorFlow Lite Micro)
if(OVE_INFER AND NOT TARGET ove_tflm)
    if(NOT DEFINED OVE_DL_DIR)
        set(OVE_DL_DIR "${OVE_DIR}/dl")
    endif()
    set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
    set(OVE_CMSIS5_DIR ${OVE_DIR}/dl/CMSIS_5/CMSIS)
    set(OVE_CMSIS_NN_DIR ${OVE_CMSIS5_DIR}/NN)
    include(${OVE_DIR}/cmake/OveTflm.cmake)
    ove_build_tflm()
    target_include_directories(ove_tflm PRIVATE
        ${OVE_DIR}/backends/zephyr/include)
    target_link_libraries(ove_tflm PRIVATE zephyr_interface)
    # Add ove_tflm objects directly to app rather than as a separate
    # library.  This avoids link-order issues where Rust/Zig bindings
    # (linked last) reference ove_tflm symbols.
    target_sources(app PRIVATE $<TARGET_OBJECTS:ove_tflm>)
    target_include_directories(app PRIVATE $<TARGET_PROPERTY:ove_tflm,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(app PRIVATE stdc++)
endif()

# Application code (generated from app.yaml by 'ove configure')
if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
    include(${OVE_GEN_DIR}/app_sources.cmake)
elseif(EXISTS "${OVE_APP_DIR}/CMakeLists.txt")
    message(WARNING "Using legacy app CMakeLists.txt — migrate to app.yaml")
    include(${OVE_APP_DIR}/CMakeLists.txt)
else()
    message(FATAL_ERROR "No app build description found. Run 'ove configure' first.")
endif()
include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)
ove_apply_app_language(app)

target_include_directories(app PRIVATE
    ${OVE_GEN_DIR}
    ${APP_INCLUDE_DIRS}
    ${OVE_DIR}/include
    ${OVE_DIR}/bindings/cpp
    ${OVE_DIR}/backends/zephyr/include
    ${OVE_DIR}/backends/common
)
