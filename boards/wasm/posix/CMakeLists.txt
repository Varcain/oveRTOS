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
project(ove_wasm LANGUAGES C CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OvePosixCommon.cmake)

# -pthread must be on the compile line BEFORE any LVGL source gets GLOBbed
# — Emscripten pthreads have compile-time semantics and Rust bindgen needs
# the flag visible to every translation unit.
add_compile_options(-pthread)

ove_posix_setup()
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

# Sampling profiler needs USE_OFFSET_CONVERTER so
# emscripten_get_callstack() can map wasm bytecode offsets back to C
# function names (via the wasm `name` custom section). Without this the
# runtime returns anonymous frames and the parser emits depth=0 samples
# — the profiler panel shows "Waiting for samples…" forever.
#
# --profiling-funcs preserves the wasm `name` custom section with
# minimal size overhead even when DWARF is stripped (OVE_DEBUG=n).
# `-g1` is NOT sufficient here: in modern emscripten it only preserves
# JS-side names, not the wasm name section, so the offset converter
# resolves offsets to empty strings.
if(OVE_PROFILER)
    add_link_options(
        -sUSE_OFFSET_CONVERTER=1
        --profiling-funcs
    )
endif()

#############################################################################
# WASM debug: embed DWARF + emit sourcemap so Chrome DevTools (with the
# "C/C++ DevTools Support (DWARF)" extension) can step through C source,
# set breakpoints in .c files, and inspect named locals.  Compile-side
# `-g` is already added by OvePosixCommon.cmake; Emscripten needs `-g`
# on the link line too to retain DWARF in the final .wasm.
#
# DevTools URL resolution for DWARF sources (per Chromium's C/C++
# extension):
#   1. DW_AT_name starting with a scheme or `/`  → used as-is; a
#      leading-slash path becomes a file:// URL, which Chromium
#      refuses to load from an http:// origin.
#   2. Otherwise DW_AT_name is resolved against DW_AT_comp_dir as a
#      URL; if that is still relative, it is resolved against the
#      wasm module's URL (i.e. http://localhost:8080/ove_wasm.wasm).
#
# To land paths at http://localhost:8080/apps/c/example/src/app.c:
#   - strip the absolute OVE_DIR prefix from DW_AT_name via
#     -ffile-prefix-map so they become repo-relative
#     (`apps/c/example/src/app.c`);
#   - force DW_AT_comp_dir to `.` with -fdebug-compilation-dir so the
#     comp_dir + name join is itself relative (not the CMake build
#     directory, which would yield `output/.../build/firmware/apps/`).
#
# The dev server's translate_path falls back to the repo root, so
# these repo-relative URLs serve directly from the on-disk tree.
#############################################################################
if(OVE_DEBUG)
    add_compile_options(
        -ffile-prefix-map=${OVE_DIR}/=
        -fdebug-compilation-dir=.
    )
    add_link_options(
        -g -gsource-map
        -ffile-prefix-map=${OVE_DIR}/=
    )
endif()

#############################################################################
# Optional WASM safety-net: runtime traps for heap/stack/alignment errors.
# Orthogonal to OVE_DEBUG — can be enabled with release builds too.  Pays
# a significant runtime cost, so default OFF.
#############################################################################
option(OVE_WASM_SAFE "Enable Emscripten runtime safety checks (SAFE_HEAP, ASSERTIONS, stack overflow)" OFF)
if(OVE_WASM_SAFE)
    add_compile_options(-sSAFE_HEAP=1 -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2)
    add_link_options(-sSAFE_HEAP=1 -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2)
    message(STATUS "  WASM safety: ON (SAFE_HEAP, ASSERTIONS=2, STACK_OVERFLOW_CHECK=2)")
endif()

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
        ${OVE_DIR}/sim/hal/sim_trace.c
        ${OVE_DIR}/sim/hal/sim_profiler.c
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
