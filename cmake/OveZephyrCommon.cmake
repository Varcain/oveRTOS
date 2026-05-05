# ============================================================================
# OveZephyrCommon.cmake — Shared build infrastructure for Zephyr boards
# ============================================================================
#
# Used by boards/<board>/zephyr/CMakeLists.txt.  Zephyr's build system is
# sensitive to ordering around find_package(Zephyr): DTS_ROOT and
# ZEPHYR_EXTRA_MODULES must be set before find_package runs; target_*
# calls on `app` must come after it.  This file provides helpers for both
# phases:
#
#   ove_zephyr_pre_find()                — before find_package(Zephyr)
#   ove_zephyr_add_tflm_if_enabled()     — after project()
#   ove_zephyr_include_app_sources()     — after project()
#   ove_zephyr_add_common_includes()     — after project()
#   ove_zephyr_copy_lv_conf_for_bindgen() — optional Rust bindgen helper
#
# The board file itself must call find_package(Zephyr) and project() —
# Zephyr's build system requires these to live in the top-level
# CMakeLists.txt.
#
# Typical usage:
#
#   cmake_minimum_required(VERSION 3.20.0)
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZephyrCommon.cmake)
#   ove_zephyr_pre_find()
#   list(APPEND ZEPHYR_EXTRA_MODULES ${OVE_BOARD_DIR}/drivers)  # optional
#   find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
#   project(ove_firmware)
#   target_sources(app PRIVATE ${OVE_BOARD_DIR}/main.c ${OVE_BACKEND_SOURCES})
#   ove_zephyr_add_tflm_if_enabled()
#   ove_zephyr_include_app_sources()
#   ove_zephyr_add_common_includes()

cmake_minimum_required(VERSION 3.20.0)

# ─── ove_zephyr_pre_find() ──────────────────────────────────────────
# Runs before find_package(Zephyr).  Resolves paths, includes the
# generated ove_config.cmake, and adds the board directory to DTS_ROOT
# so board-local DTS bindings are picked up.
macro(ove_zephyr_pre_find)
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

    # Download directory (workspace-isolated).
    # OVE_GEN_DIR is <workspace>/generated; dl/ lives alongside it.
    if(NOT DEFINED OVE_DL_DIR)
        get_filename_component(OVE_DL_DIR "${OVE_GEN_DIR}/../dl" ABSOLUTE)
    endif()

    # Board-local DTS bindings
    list(APPEND DTS_ROOT ${OVE_BOARD_DIR})
endmacro()


# ─── ove_zephyr_copy_lv_conf_for_bindgen() ──────────────────────────
# Places lv_conf.h at the location LVGL's lv_conf_internal.h expects via
# a relative #include "../../lv_conf.h" from lvgl/src/.  This is required
# for Rust bindgen, which processes LVGL headers with its own clang and
# doesn't see the CMake-level LV_CONF_PATH.  Safe to call unconditionally;
# no-op when ZEPHYR_BASE is unset or the target already exists.
macro(ove_zephyr_copy_lv_conf_for_bindgen)
    if(DEFINED ZEPHYR_BASE AND EXISTS "${OVE_BOARD_DIR}/lv_conf.h")
        set(_LVGL_CONF_DEST "${ZEPHYR_BASE}/../modules/lib/gui/lv_conf.h")
        if(NOT EXISTS "${_LVGL_CONF_DEST}")
            file(COPY "${OVE_BOARD_DIR}/lv_conf.h"
                 DESTINATION "${ZEPHYR_BASE}/../modules/lib/gui")
        endif()
    endif()
endmacro()


# ─── ove_zephyr_add_tflm_if_enabled() ───────────────────────────────
# Build TFLM when OVE_INFER is set, and wire it into `app` via
# $<TARGET_OBJECTS:ove_tflm> so Rust/Zig bindings (linked last) can
# reference its symbols.  Must be called after project().
macro(ove_zephyr_add_tflm_if_enabled)
    if(OVE_INFER AND NOT TARGET ove_tflm)
        if(NOT DEFINED OVE_DL_DIR)
            set(OVE_DL_DIR "${OVE_DIR}/dl")
        endif()
        set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
        set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
        include(${OVE_DIR}/cmake/OveTflm.cmake)
        ove_build_tflm()
        target_include_directories(ove_tflm PRIVATE
            ${OVE_DIR}/backends/zephyr/include)
        target_link_libraries(ove_tflm PRIVATE zephyr_interface)
        # Add ove_tflm objects directly to app rather than as a separate
        # library.  This avoids link-order issues where Rust/Zig bindings
        # (linked last) reference ove_tflm symbols.
        target_sources(app PRIVATE $<TARGET_OBJECTS:ove_tflm>)
        target_include_directories(app PRIVATE
            $<TARGET_PROPERTY:ove_tflm,INTERFACE_INCLUDE_DIRECTORIES>)
        target_link_libraries(app PRIVATE stdc++)
    endif()
endmacro()


# ─── ove_zephyr_include_app_sources() ───────────────────────────────
# Include the generated app_sources.cmake (from 'ove configure') and
# apply the application language layer (C / C++ / Rust / Zig).  Must be
# called after project().
macro(ove_zephyr_include_app_sources)
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
endmacro()


# ─── ove_zephyr_add_profiler_flags() ────────────────────────────────
# When OVE_PROFILER is set, Zephyr's sampling backend scans the task
# stack for saved-{r7, lr} pairs pushed by Thumb-2 prologues — same
# mechanism FreeRTOS uses. That requires -fno-omit-frame-pointer on the
# app's C/C++ sources so the compiler emits those pushes. Harmless when
# the profiler is off; harmless on non-ARM Zephyr targets.
macro(ove_zephyr_add_profiler_flags)
    if(OVE_PROFILER)
        zephyr_compile_options(-fno-omit-frame-pointer)
    endif()
endmacro()


# ─── ove_zephyr_add_common_includes([EXTRA <dirs>...]) ──────────────
# Add the common include directories to `app`.  Boards can pass EXTRA
# <dirs>... to add more.
macro(ove_zephyr_add_common_includes)
    cmake_parse_arguments(_OVE_ZI "" "" "EXTRA" ${ARGN})
    target_include_directories(app PRIVATE
        ${OVE_GEN_DIR}
        ${APP_INCLUDE_DIRS}
        ${OVE_DIR}/include
        ${OVE_DIR}/bindings/cpp
        ${OVE_DIR}/backends/zephyr/include
        ${OVE_DIR}/backends/common
        ${_OVE_ZI_EXTRA}
    )

    # ETL — header-only fixed-capacity containers for C++ apps.  Added
    # to `app` only so it's not paid for in C-only builds.  No-op when
    # the dl/etl tree is absent.
    if(OVE_APP_LANG STREQUAL "cpp" AND EXISTS "${OVE_DL_DIR}/etl/include/etl/vector.h")
        target_include_directories(app PRIVATE ${OVE_DL_DIR}/etl/include)
        target_compile_definitions(app PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:ETL_NO_EXCEPTIONS>)
    endif()

    # Picolibc's <stdlib.h> guards malloc/calloc/free/realloc behind
    # `__POSIX_VISIBLE >= 200809`.  GCC's `-std=c++20` (strict, set by
    # Zephyr's compiler-cpp dialect) predefines __STRICT_ANSI__, which
    # forces picolibc's sys/cdefs.h to set __POSIX_VISIBLE == 0 — so
    # libstdc++'s <cstdlib> can't find `::calloc`/`::free`/`::malloc`/
    # `::realloc` and rejects every C++ TU that pulls in <memory>.
    # Undefining __STRICT_ANSI__ tells picolibc to honour
    # _POSIX_C_SOURCE / _DEFAULT_SOURCE, then we set the POSIX level
    # high enough to expose the malloc family.  Switching to
    # `-std=gnu++20` instead would also work but Zephyr's
    # CONFIG_STD_CPP20 hard-codes the strict dialect.
    if(OVE_APP_LANG STREQUAL "cpp")
        target_compile_options(app PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-U__STRICT_ANSI__>)
        target_compile_definitions(app PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:_POSIX_C_SOURCE=200809L>)
    endif()
endmacro()
