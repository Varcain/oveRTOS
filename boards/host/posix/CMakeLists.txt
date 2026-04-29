# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.14)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OvePosixCommon.cmake)

ove_posix_setup_project(NAME ove_posix)
ove_posix_include_app_sources()
ove_posix_add_common_includes()

# Sim framework include paths (host always uses the sim dashboard)
include_directories(
    ${OVE_DIR}/sim/include
    ${OVE_DIR}/sim/src
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
        ${OVE_DIR}/backends/posix/posix_thread.c
        ${OVE_DIR}/backends/posix/posix_sync.c
        ${OVE_DIR}/backends/posix/posix_fs.c
        ${OVE_DIR}/backends/posix/posix_console.c
        ${OVE_DIR}/backends/posix/posix_time.c
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
# Simulation plugin framework (display + audio via browser dashboard)
#############################################################################
include(${OVE_DIR}/sim/CMakeLists.txt)
# Remove POSIX backends that the sim framework replaces
list(FILTER OVE_SOURCES EXCLUDE REGEX "posix_console\\.c$")
if(OVE_LVGL)
    list(FILTER OVE_SOURCES EXCLUDE REGEX "posix_lvgl\\.c$")
endif()
if(OVE_AUDIO)
    list(FILTER OVE_SOURCES EXCLUDE REGEX "posix_audio\\.c$")
endif()
list(APPEND OVE_SOURCES ${SIM_SOURCES})

#############################################################################
# Optional libraries
#############################################################################
ove_posix_build_tflm_if_enabled()

if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

#############################################################################
# Final executable
#############################################################################
ove_posix_link(
    TARGET ove_posix
    SOURCES ${BOARD_DIR}/main.c ${OVE_SOURCES}
    EXTRA_LIBS pthread rt m
)

#############################################################################
# Benchmark apps (live under tests/benchmarks/<lang>/): force -O2 +
# -fno-omit-frame-pointer.  Host POSIX selects OVE_DEBUG_BUILD by default
# (-> -g -O0), but the README's "zero overhead" claim only holds at
# production optimization — under -O0 the binding wrappers don't inline.
# Override the directory-level -O0 with a target-level -O2 so both the
# static audit and the runtime measurements reflect a real release build.
# Frame pointers stay on for the sim profiler's backtrace walker.
#
# Static + hotpath audits also run as POST_BUILD steps on the benchmark
# binaries — proves no vtables/typeinfo/dispatch-tables leaked into any
# binding, and that wrapper layers compile to the expected inlined call
# sequence.
#############################################################################
if(OVE_APP_DIR MATCHES "/tests/benchmarks/")
    target_compile_options(ove_posix PRIVATE -O2 -fno-omit-frame-pointer)
    target_compile_definitions(ove_posix PRIVATE
        OVE_APP_LANG_NAME=\"${OVE_APP_LANG}\")
    include(${OVE_DIR}/cmake/OveZeroOverheadAudit.cmake)
    ove_assert_no_dispatch_overhead(ove_posix BINDING ${OVE_APP_LANG})
    ove_dump_hotpaths(ove_posix BINDING ${OVE_APP_LANG})
endif()

#############################################################################
# Print configuration
#############################################################################
message(STATUS "oveRTOS POSIX build (browser dashboard)")
message(STATUS "  OVE_DIR: ${OVE_DIR}")
message(STATUS "  APP_DIR: ${OVE_APP_DIR}")
message(STATUS "  LVGL:    ${LVGL_PATH}")
