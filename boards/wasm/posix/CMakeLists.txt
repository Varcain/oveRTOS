# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# WASM/Emscripten build.
#
# Uses Emscripten's pthread support (SharedArrayBuffer) for the oveRTOS
# threading model.  Replaces the POSIX thread/timer/time/console backends
# with WASM-compatible versions.  Reuses POSIX sync/queue/etc as-is.
#
# Build:
#   emcmake cmake -B build boards/wasm/posix
#   emmake make -C build

cmake_minimum_required(VERSION 3.14)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OvePosixCommon.cmake)

# -pthread must be on the compile line BEFORE any LVGL source gets GLOBbed
# — Emscripten pthreads have compile-time semantics and Rust bindgen needs
# the flag visible to every translation unit.
add_compile_options(-pthread)

ove_posix_setup_project(NAME ove_wasm)
ove_posix_include_app_sources()
ove_posix_add_common_includes()

# WASM-specific backend include path
include_directories(${OVE_DIR}/backends/wasm/include)

if(OVE_SIM)
    include_directories(
        ${OVE_DIR}/sim/include
        ${OVE_DIR}/sim/src
    )
endif()

#############################################################################
# Emscripten link flags
#############################################################################
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

ove_posix_build_lvgl()

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
        ${OVE_DIR}/sim/src/ove_sim_input.c
        ${OVE_DIR}/sim/src/ove_sim_transport_wasm.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_fb.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_input.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_audio.c
        ${OVE_DIR}/sim/src/ove_sim_wasm_config.c
        ${OVE_DIR}/sim/hal/sim_board.c
        ${OVE_DIR}/sim/hal/sim_debug.c
    )
    if(OVE_LVGL)
        list(APPEND SIM_SOURCES
            ${OVE_DIR}/sim/hal/sim_lvgl.c
            ${OVE_DIR}/sim/hal/sim_display.c
        )
    endif()
    if(OVE_AUDIO)
        list(APPEND SIM_SOURCES ${OVE_DIR}/sim/hal/sim_audio.c)
    endif()
    if(OVE_NET)
        list(APPEND SIM_SOURCES ${OVE_DIR}/sim/hal/sim_net.c)
    endif()
    if(OVE_NET_TLS)
        list(APPEND SIM_SOURCES ${OVE_DIR}/sim/hal/sim_tls.c)
    endif()
    # Browser-native HTTP (emscripten_fetch) and SNTP (Date.now)
    if(OVE_NET_HTTP)
        list(APPEND SIM_SOURCES ${OVE_DIR}/backends/wasm/wasm_http.c)
    endif()
    if(OVE_NET_SNTP)
        list(APPEND SIM_SOURCES ${OVE_DIR}/backends/wasm/wasm_sntp.c)
    endif()
    if(OVE_PM)
        list(APPEND SIM_SOURCES ${OVE_DIR}/sim/hal/sim_pm.c)
    endif()
    if(OVE_WATCHDOG)
        list(APPEND SIM_SOURCES ${OVE_DIR}/sim/hal/sim_watchdog.c)
    endif()
    list(APPEND OVE_SOURCES ${SIM_SOURCES})
endif()

ove_posix_build_tflm_if_enabled()

#############################################################################
# Final executable
#############################################################################
ove_posix_link(
    TARGET ove_wasm
    SOURCES ${BOARD_DIR}/main.c ${OVE_SOURCES}
    EXTRA_LIBS pthread m
)

# Output: ove_wasm.html, ove_wasm.js, ove_wasm.wasm, ove_wasm.worker.js
set_target_properties(ove_wasm PROPERTIES SUFFIX ".html")

message(STATUS "oveRTOS WASM/Emscripten build")
message(STATUS "  OVE_DIR: ${OVE_DIR}")
message(STATUS "  LVGL:    ${LVGL_PATH}")
