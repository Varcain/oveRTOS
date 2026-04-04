# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# WASM/Emscripten build.
#
# Uses Emscripten's pthread support (SharedArrayBuffer) for the oveRTOS
# threading model.  Replaces the POSIX thread/timer/time backends with
# WASM-compatible versions.  Reuses POSIX sync/queue/etc as-is.
#
# Build:
#   emcmake cmake -B build boards/wasm/posix
#   emmake make -C build

cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED OVE_DIR)
    get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()
include(${OVE_DIR}/cmake/OveAppResolve.cmake)
if(NOT DEFINED OVE_GEN_DIR)
    set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
endif()
if(NOT DEFINED OVE_DL_DIR)
    message(FATAL_ERROR "OVE_DL_DIR must be set")
endif()

set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})
set(LVGL_PATH ${OVE_DL_DIR}/lvgl)

if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
    include(${OVE_GEN_DIR}/ove_config.cmake)
endif()

# Enable CXX if app is C++ or ML inference is on (TFLM is C++)
if(OVE_APP_LANG STREQUAL "cpp" OR OVE_INFER)
    project(ove_wasm C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
else()
    project(ove_wasm C)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

#############################################################################
# Emscripten flags
#############################################################################
# Emscripten: compile-time flags (pthreads must be known at compile time)
add_compile_options(-pthread)

# Emscripten: link-time flags
add_link_options(
    -sUSE_PTHREADS=1
    -sPTHREAD_POOL_SIZE=16
    -sPROXY_TO_PTHREAD=1
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=33554432
    -sMAXIMUM_MEMORY=134217728
    -sEXPORTED_FUNCTIONS=['_main','_malloc','_free','_ove_sim_wasm_push_cmd','_ove_wasm_fb_get_seq','_ove_wasm_fb_get_width','_ove_wasm_fb_get_height','_ove_wasm_fb_get_size','_ove_wasm_fb_get_pixels','_ove_wasm_fb_get_ptr','_ove_wasm_input_set','_ove_wasm_console_push','_ove_wasm_audio_get_playback_ptr','_ove_wasm_audio_get_capture_ptr','_ove_wasm_audio_playback_available','_ove_wasm_audio_capture_available','_ove_wasm_audio_set_playback_fmt','_ove_wasm_audio_set_capture_fmt','_ove_wasm_has_audio','_ove_wasm_has_lvgl','_ove_wasm_has_shell','_ove_wasm_has_net','_ove_wasm_has_infer']
    -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAPU32']
    -sENVIRONMENT=web,worker
    -sEXIT_RUNTIME=0
    -sFETCH=1
    --shell-file=${CMAKE_CURRENT_LIST_DIR}/../shell.html
)

#############################################################################
# Application sources
#############################################################################
if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
    include(${OVE_GEN_DIR}/app_sources.cmake)
endif()

#############################################################################
# Include directories
#############################################################################
include_directories(
    ${OVE_GEN_DIR}
    ${APP_INCLUDE_DIRS}
    ${OVE_DIR}/include
    ${OVE_DIR}/bindings/cpp
    ${OVE_DIR}/backends/wasm/include
    ${OVE_DIR}/backends/posix/include
    ${OVE_DIR}/backends/common
    ${BOARD_DIR}
    ${LVGL_PATH}
    ${OVE_DL_DIR}
)

if(OVE_SIM)
    include_directories(
        ${OVE_DIR}/sim/include
        ${OVE_DIR}/sim/src
    )
endif()

#############################################################################
# LVGL library
#############################################################################
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_PATH}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC
    ${LVGL_PATH}
    ${OVE_DL_DIR}
    ${BOARD_DIR}
)
target_compile_options(lvgl PRIVATE -Wno-unused-parameter)
target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)

#############################################################################
# oveRTOS backend sources
#############################################################################
if(DEFINED OVE_BACKEND_SOURCES)
    set(OVE_SOURCES ${OVE_BACKEND_SOURCES})
else()
    set(OVE_SOURCES
        ${OVE_DIR}/src/ove_app.c
        # WASM-specific backends (replace posix thread/timer/time)
        ${OVE_DIR}/backends/wasm/wasm_thread.c
        ${OVE_DIR}/backends/wasm/wasm_timer.c
        ${OVE_DIR}/backends/wasm/wasm_time.c
        # Reuse POSIX backends (all pthread-based, Emscripten compatible)
        ${OVE_DIR}/backends/posix/posix_sync.c
        ${OVE_DIR}/backends/posix/posix_queue.c
        ${OVE_DIR}/backends/posix/posix_eventgroup.c
        ${OVE_DIR}/backends/wasm/wasm_console.c
        ${OVE_DIR}/backends/posix/posix_workqueue.c
        ${OVE_DIR}/backends/posix/posix_stream.c
        ${OVE_DIR}/backends/posix/posix_watchdog.c
        ${OVE_DIR}/backends/posix/posix_nvs.c
        ${OVE_DIR}/backends/posix/posix_shell.c
        ${OVE_DIR}/backends/posix/posix_bsp.c
        # WASM stubs for bus drivers (networking handled by sim_net)
        ${OVE_DIR}/backends/wasm/wasm_bus_stubs.c
    )
endif()

#############################################################################
# Sim framework (with WASM transport, no WS server)
#############################################################################
if(OVE_SIM)
    set(SIM_SOURCES
        ${OVE_DIR}/sim/src/ove_sim_registry.c
        ${OVE_DIR}/sim/src/ove_sim_transport_wasm.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_fb.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_input.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_audio.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_config.c
        ${OVE_DIR}/sim/hal/sim_board.c
    )
    if(OVE_LVGL)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_lvgl.c
            ${OVE_DIR}/sim/hal/sim_display.c
        )
    endif()
    if(OVE_AUDIO)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_audio.c
        )
    endif()
    if(OVE_NET)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_net.c
        )
    endif()
    if(OVE_NET_TLS)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_tls.c
        )
    endif()
    # Browser-native HTTP (emscripten_fetch) and SNTP (Date.now)
    if(OVE_NET_HTTP)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/backends/wasm/wasm_http.c
        )
    endif()
    if(OVE_NET_SNTP)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/backends/wasm/wasm_sntp.c
        )
    endif()
    if(OVE_PM)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_pm.c
        )
    endif()
    if(OVE_WATCHDOG)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_watchdog.c
        )
    endif()
    list(APPEND OVE_SOURCES ${SIM_SOURCES})
endif()

#############################################################################
# ML inference (TensorFlow Lite Micro)
#############################################################################
if(OVE_INFER)
    set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
    set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
    include(${OVE_DIR}/cmake/OveTflm.cmake)
    ove_build_tflm()
endif()

#############################################################################
# Executable
#############################################################################
include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)

add_executable(ove_wasm
    ${BOARD_DIR}/main.c
    ${OVE_SOURCES}
)
ove_apply_app_language(ove_wasm)

target_compile_definitions(ove_wasm PRIVATE
    LV_CONF_INCLUDE_SIMPLE
)

target_link_libraries(ove_wasm PRIVATE
    lvgl
    $<$<BOOL:${OVE_INFER}>:ove_tflm>
    pthread
    m
)

# Output: ove_wasm.html, ove_wasm.js, ove_wasm.wasm, ove_wasm.worker.js
set_target_properties(ove_wasm PROPERTIES SUFFIX ".html")

message(STATUS "oveRTOS WASM/Emscripten build")
message(STATUS "  OVE_DIR: ${OVE_DIR}")
message(STATUS "  LVGL: ${LVGL_PATH}")
