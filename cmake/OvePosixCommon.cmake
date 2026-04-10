# ============================================================================
# OvePosixCommon.cmake — Shared build infrastructure for POSIX / WASM boards
# ============================================================================
#
# Used by boards/host/posix/ and boards/wasm/posix/.  Encapsulates path
# resolution, LVGL library build, app source inclusion, TFLM integration,
# and final link step.  Emscripten-specific link options, the choice of
# sim framework sources, and the posix/wasm backend source list stay
# board-local (they differ meaningfully between host and wasm).
#
# Typical usage (boards/host/posix/CMakeLists.txt):
#
#   cmake_minimum_required(VERSION 3.14)
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OvePosixCommon.cmake)
#   ove_posix_setup_project(NAME ove_posix)
#   ove_posix_add_common_includes()
#   ove_posix_build_lvgl()
#   # ...board-specific backend list + sim framework integration...
#   ove_posix_build_tflm_if_enabled()
#   ove_posix_link(TARGET ove_posix SOURCES ${BOARD_DIR}/main.c ${OVE_SOURCES}
#       EXTRA_LIBS pthread rt m)

cmake_minimum_required(VERSION 3.14)

# ─── ove_posix_setup_project(NAME <name>) ───────────────────────────
# Resolve paths, include generated config, declare the CMake project with
# CXX detection, and set common C flags.  Must be called first from the
# board CMakeLists.txt.
macro(ove_posix_setup_project)
    cmake_parse_arguments(_OVE_PS "" "NAME" "" ${ARGN})
    if(NOT _OVE_PS_NAME)
        message(FATAL_ERROR "ove_posix_setup_project: NAME required")
    endif()
    set(_OVE_POSIX_NAME "${_OVE_PS_NAME}")

    if(NOT DEFINED OVE_DIR)
        get_filename_component(OVE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    endif()
    include(${OVE_DIR}/cmake/OveAppResolve.cmake)
    if(NOT DEFINED OVE_GEN_DIR)
        set(OVE_GEN_DIR ${OVE_DIR}/output/generated)
    endif()
    if(NOT DEFINED OVE_DL_DIR)
        message(FATAL_ERROR
            "OVE_DL_DIR must be set by the build system "
            "(e.g. -DOVE_DL_DIR=<workspace>/dl)")
    endif()

    set(BOARD_DIR ${CMAKE_CURRENT_LIST_DIR})
    set(LVGL_PATH ${OVE_DL_DIR}/lvgl)

    # Include generated CMake config (sets OVE_RTOS, OVE_APP_LANG, etc.)
    if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
        include(${OVE_GEN_DIR}/ove_config.cmake)
    endif()

    # Include CXX if app language requires it, or if ML inference is
    # enabled (TFLM is C++).
    if(OVE_APP_LANG STREQUAL "cpp" OR OVE_INFER)
        project(${_OVE_POSIX_NAME} C CXX)
        set(CMAKE_CXX_STANDARD 17)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    else()
        project(${_OVE_POSIX_NAME} C)
    endif()

    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endmacro()


# ─── ove_posix_add_common_includes() ────────────────────────────────
# Add the include directories shared between host and wasm builds.
# Boards can add more include directories after calling this.
macro(ove_posix_add_common_includes)
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
    )
endmacro()


# ─── ove_posix_include_app_sources() ────────────────────────────────
# Include the generated app_sources.cmake (from 'ove configure'), with
# fallbacks to legacy per-app CMakeLists.txt.  Populates APP_SOURCES and
# APP_INCLUDE_DIRS.
macro(ove_posix_include_app_sources)
    if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
        include(${OVE_GEN_DIR}/app_sources.cmake)
    elseif(EXISTS "${OVE_APP_DIR}/CMakeLists.txt")
        message(WARNING "Using legacy app CMakeLists.txt — migrate to app.yaml")
        include(${OVE_APP_DIR}/CMakeLists.txt)
    else()
        message(FATAL_ERROR "No app build description found. Run 'ove configure' first.")
    endif()
endmacro()


# ─── ove_posix_build_lvgl() ─────────────────────────────────────────
# Build LVGL as a static library from dl/lvgl/src/*.c.  Include paths are
# exported PUBLIC so the final executable sees lv_conf.h from BOARD_DIR.
macro(ove_posix_build_lvgl)
    file(GLOB_RECURSE _LVGL_SOURCES ${LVGL_PATH}/src/*.c)
    add_library(lvgl STATIC ${_LVGL_SOURCES})
    target_include_directories(lvgl SYSTEM PUBLIC
        ${LVGL_PATH}
        ${OVE_DL_DIR}
        ${BOARD_DIR}
    )
    target_compile_options(lvgl PRIVATE -Wno-unused-parameter)
    target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)
endmacro()


# ─── ove_posix_build_tflm_if_enabled() ──────────────────────────────
# Build TFLM as a static library if OVE_INFER is set.
macro(ove_posix_build_tflm_if_enabled)
    if(OVE_INFER AND NOT TARGET ove_tflm)
        set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
        set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
        include(${OVE_DIR}/cmake/OveTflm.cmake)
        ove_build_tflm()
    endif()
endmacro()


# ─── ove_posix_link(TARGET <name> SOURCES <srcs> [EXTRA_LIBS <libs>]) ─
# Create the final executable, apply the app language layer, and link
# the common libraries (lvgl, optional ove_tflm/ove_mbedtls, plus any
# EXTRA_LIBS supplied by the board — typically pthread/rt/m for host
# and pthread/m for wasm).
macro(ove_posix_link)
    cmake_parse_arguments(_OVE_PL "" "TARGET" "SOURCES;EXTRA_LIBS" ${ARGN})
    if(NOT _OVE_PL_TARGET)
        message(FATAL_ERROR "ove_posix_link: TARGET required")
    endif()
    if(NOT _OVE_PL_SOURCES)
        message(FATAL_ERROR "ove_posix_link: SOURCES required")
    endif()

    include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)

    add_executable(${_OVE_PL_TARGET} ${_OVE_PL_SOURCES})
    ove_apply_app_language(${_OVE_PL_TARGET})

    target_compile_definitions(${_OVE_PL_TARGET} PRIVATE
        LV_CONF_INCLUDE_SIMPLE
    )

    target_link_libraries(${_OVE_PL_TARGET} PRIVATE
        lvgl
        $<$<BOOL:${OVE_INFER}>:ove_tflm>
        $<$<BOOL:${OVE_NET_TLS}>:ove_mbedtls>
        ${_OVE_PL_EXTRA_LIBS}
    )
endmacro()
