# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)

# ============================================================================
# Path configuration — set by oveRTOS top-level Makefile
# ============================================================================

if(NOT DEFINED OVE_DIR)
    get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
if(NOT DEFINED OVE_APP_DIR)
    file(STRINGS "${OVE_DIR}/.config" _app_line REGEX "^CONFIG_OVE_APP_NAME=")
    if(_app_line)
        string(REGEX REPLACE ".*=\"(.*)\"" "\\1" _app_name "${_app_line}")
        set(OVE_APP_DIR "${OVE_DIR}/apps/${_app_name}")
    endif()
endif()
if(NOT DEFINED OVE_GEN_DIR)
    set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
endif()
if(NOT DEFINED OVE_DL_DIR)
    message(FATAL_ERROR "OVE_DL_DIR must be set by the build system (e.g. -DOVE_DL_DIR=<workspace>/dl)")
endif()

set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})
set(LVGL_PATH ${OVE_DL_DIR}/lvgl)

# Include generated CMake config
if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
    include(${OVE_GEN_DIR}/ove_config.cmake)
endif()

# Include CXX if app language requires it
if(OVE_APP_LANG STREQUAL "cpp")
    project(ove_posix C CXX)
else()
    project(ove_posix C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

#############################################################################
# Find SDL2
#############################################################################
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

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
    ${OVE_GEN_DIR}
    ${APP_INCLUDE_DIRS}
    ${OVE_DIR}/include
    ${OVE_DIR}/bindings/cpp
    ${OVE_DIR}/backends/posix/include
    ${OVE_DIR}/backends/common
    ${BOARD_DIR}
    ${LVGL_PATH}
    ${OVE_DL_DIR}
    ${SDL2_INCLUDE_DIRS}
)

#############################################################################
# LVGL library
#############################################################################
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_PATH}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC
    ${LVGL_PATH}
    ${OVE_DL_DIR}
    ${BOARD_DIR}
    ${SDL2_INCLUDE_DIRS}
)
target_compile_options(lvgl PRIVATE ${SDL2_CFLAGS_OTHER} -Wno-unused-parameter)
target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)

#############################################################################
# oveRTOS backend sources
#############################################################################
if(DEFINED OVE_BACKEND_SOURCES)
    set(OVE_SOURCES ${OVE_BACKEND_SOURCES})
else()
    set(OVE_SOURCES
        ${OVE_DIR}/src/ove_app.c
        ${OVE_DIR}/backends/posix/posix_thread.c
        ${OVE_DIR}/backends/posix/posix_sync.c
        ${OVE_DIR}/backends/posix/posix_audio.c
        ${OVE_DIR}/backends/posix/posix_fs.c
        ${OVE_DIR}/backends/posix/posix_console.c
        ${OVE_DIR}/backends/posix/posix_time.c
        ${OVE_DIR}/backends/posix/posix_lvgl.c
        ${OVE_DIR}/backends/posix/posix_queue.c
        ${OVE_DIR}/backends/posix/posix_timer.c
        ${OVE_DIR}/backends/posix/posix_eventgroup.c
        ${OVE_DIR}/backends/posix/posix_workqueue.c
        ${OVE_DIR}/backends/posix/posix_stream.c
        ${OVE_DIR}/backends/posix/posix_watchdog.c
        ${OVE_DIR}/backends/posix/posix_nvs.c
        ${OVE_DIR}/backends/posix/posix_shell.c
        ${OVE_DIR}/backends/posix/posix_bsp.c
    )
endif()

#############################################################################
# Executable
#############################################################################
#############################################################################
# Application language support
#############################################################################
include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)

add_executable(ove_posix
    ${BOARD_DIR}/main.c
    ${OVE_SOURCES}
)
ove_apply_app_language(ove_posix)

target_compile_definitions(ove_posix PRIVATE
    LV_CONF_INCLUDE_SIMPLE
)

target_link_libraries(ove_posix PRIVATE
    lvgl
    ${SDL2_LIBRARIES}
    pthread
    rt
    m
)

#############################################################################
# Print configuration
#############################################################################
message(STATUS "oveRTOS POSIX/SDL2 build")
message(STATUS "  OVE_DIR: ${OVE_DIR}")
message(STATUS "  APP_DIR: ${OVE_APP_DIR}")
message(STATUS "  LVGL: ${LVGL_PATH}")
message(STATUS "  SDL2: ${SDL2_LIBRARIES}")
