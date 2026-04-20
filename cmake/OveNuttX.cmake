# ============================================================================
# OveNuttX.cmake — NuttX CMake integration for oveRTOS external applications
# ============================================================================
#
# Runs within NuttX's CMake build system.  The board-level CMakeLists.txt
# (copied to nuttx-apps/external/ove_app/) includes this module and uses
# its macros to register the oveRTOS app via nuttx_add_application().
#
# Unlike OveCommon.cmake (which drives a standalone build for FreeRTOS,
# Zephyr and POSIX), this module must work inside NuttX's project()
# context — no project() declaration, no toolchain file, no linker
# script handling.
#
# Usage (in boards/<board>/nuttx/CMakeLists.txt):
#   include(${OVE_DIR}/cmake/OveNuttX.cmake)
#   ove_nuttx_setup()
#   ove_nuttx_add_backend_sources()
#   ove_nuttx_exclude_backends(LVGL AUDIO)
#   ove_nuttx_add_sources(${SIM_SOURCES})
#   ove_nuttx_build_lvgl()
#   ove_nuttx_register_app()

cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/OveHelpers.cmake)


# ─── ove_nuttx_setup() ─────────────────────────────────────────────
# Resolve paths, include generated config, initialise accumulators.
# Call once at the top of the board CMakeLists.txt.
macro(ove_nuttx_setup)
    # Resolve OVE_DIR from environment (set by build.py)
    if(DEFINED ENV{OVE_DIR} AND NOT DEFINED OVE_DIR)
        set(OVE_DIR "$ENV{OVE_DIR}")
    endif()
    if(DEFINED ENV{OVE_GEN_DIR} AND NOT DEFINED OVE_GEN_DIR)
        set(OVE_GEN_DIR "$ENV{OVE_GEN_DIR}")
    endif()
    if(DEFINED ENV{OVE_APP_DIR} AND NOT DEFINED OVE_APP_DIR)
        set(OVE_APP_DIR "$ENV{OVE_APP_DIR}")
    endif()

    # Fallback: read .ove_env (key=value format, written by build.py)
    if(NOT DEFINED OVE_DIR)
        set(_ove_env "${CMAKE_CURRENT_SOURCE_DIR}/.ove_env")
        if(EXISTS "${_ove_env}")
            file(STRINGS "${_ove_env}" _env_lines)
            foreach(_line ${_env_lines})
                if(_line MATCHES "^([A-Za-z_]+)=(.+)$")
                    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
                endif()
            endforeach()
        endif()
    endif()

    if(NOT DEFINED OVE_DIR)
        message(FATAL_ERROR "OVE_DIR not set — run 'ove build'")
    endif()
    if(NOT DEFINED OVE_GEN_DIR)
        set(OVE_GEN_DIR "${OVE_DIR}/output/generated")
    endif()

    set(OVE_DL_DIR "$ENV{OVE_DL_DIR}")
    if(NOT OVE_DL_DIR)
        set(OVE_DL_DIR "${OVE_DIR}/dl")
    endif()

    set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

    # Generated config — sets OVE_RTOS, OVE_BACKEND_SOURCES, module flags
    if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
        include("${OVE_GEN_DIR}/ove_config.cmake")
    else()
        message(FATAL_ERROR
            "Generated config not found at ${OVE_GEN_DIR}/ove_config.cmake "
            "— run 'ove configure' first")
    endif()

    # Generated app sources — sets APP_SOURCES, APP_RUST_*, APP_ZIG_*, etc.
    if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
        include("${OVE_GEN_DIR}/app_sources.cmake")
    else()
        message(FATAL_ERROR
            "App sources not found at ${OVE_GEN_DIR}/app_sources.cmake "
            "— run 'ove configure' first")
    endif()

    # Initialise accumulator lists
    set(_OVE_NX_BACKEND_SRC "")
    set(_OVE_NX_EXTRA_SOURCES "")
    set(_OVE_NX_INCLUDE_DIRS
        ${OVE_DIR}/include
        ${OVE_DIR}/backends/nuttx/include
        ${OVE_DIR}/backends/common
        ${OVE_GEN_DIR}
        ${OVE_GEN_DIR}/generated_models
    )

    # App include paths
    if(APP_INCLUDE_DIRS)
        list(APPEND _OVE_NX_INCLUDE_DIRS ${APP_INCLUDE_DIRS})
    endif()

    # C++ bindings
    if(OVE_APP_LANG STREQUAL "cpp")
        list(APPEND _OVE_NX_INCLUDE_DIRS ${OVE_DIR}/bindings/cpp)
    endif()

    message(STATUS "oveRTOS NuttX app setup")
    message(STATUS "  OVE_DIR:  ${OVE_DIR}")
    message(STATUS "  APP_DIR:  ${OVE_APP_DIR}")
    message(STATUS "  GEN_DIR:  ${OVE_GEN_DIR}")
    message(STATUS "  DL_DIR:   ${OVE_DL_DIR}")
endmacro()


# ─── ove_nuttx_add_backend_sources() ───────────────────────────────
# Copy OVE_BACKEND_SOURCES (from ove_config.cmake) into the accumulator.
macro(ove_nuttx_add_backend_sources)
    if(DEFINED OVE_BACKEND_SOURCES)
        set(_OVE_NX_BACKEND_SRC ${OVE_BACKEND_SOURCES})
    else()
        message(WARNING "OVE_BACKEND_SOURCES not defined — run 'ove configure'")
    endif()
endmacro()


# ─── ove_nuttx_exclude_backends(MOD ...) ───────────────────────────
# Remove backend source files for the listed modules from the accumulator.
macro(ove_nuttx_exclude_backends)
    _ove_filter_backend_list(_OVE_NX_BACKEND_SRC ${ARGN})
endmacro()


# ─── ove_nuttx_add_stub_backends(MOD ...) ──────────────────────────
# Replace backend implementations with stubs and remove real backends.
macro(ove_nuttx_add_stub_backends)
    set(_stub_dir "${OVE_DIR}/tests/backends/stub")
    foreach(_mod ${ARGN})
        string(TOUPPER "${_mod}" _MOD_UPPER)
        string(TOLOWER "${_mod}" _mod_lower)

        set(_add_stub FALSE)
        if("${_MOD_UPPER}" STREQUAL "BSP" OR "${_MOD_UPPER}" STREQUAL "GPIO")
            set(_add_stub TRUE)
        elseif(OVE_${_MOD_UPPER})
            set(_add_stub TRUE)
        endif()

        if(_add_stub)
            if(EXISTS "${_stub_dir}/stub_${_mod_lower}.c")
                list(APPEND _OVE_NX_EXTRA_SOURCES
                     "${_stub_dir}/stub_${_mod_lower}.c")
            endif()
            _ove_filter_backend_list(_OVE_NX_BACKEND_SRC ${_mod})
        endif()
    endforeach()
endmacro()


# ─── ove_nuttx_add_sources(src ...) ────────────────────────────────
# Add extra source files to the accumulator.
macro(ove_nuttx_add_sources)
    list(APPEND _OVE_NX_EXTRA_SOURCES ${ARGN})
endmacro()


# ─── ove_nuttx_add_include_dirs(dir ...) ───────────────────────────
# Add extra include directories.
macro(ove_nuttx_add_include_dirs)
    list(APPEND _OVE_NX_INCLUDE_DIRS ${ARGN})
endmacro()


# ─── ove_nuttx_build_lvgl() ───────────────────────────────────────
# Build LVGL as a static library and register it with NuttX's linker.
macro(ove_nuttx_build_lvgl)
    if(OVE_LVGL)
        set(_LVGL_PATH "${OVE_DL_DIR}/lvgl")
        if(EXISTS "${_LVGL_PATH}/src")
            file(GLOB_RECURSE _LVGL_SOURCES CONFIGURE_DEPENDS "${_LVGL_PATH}/src/*.c")
            # Exclude Helium/NEON SIMD blends (Cortex-M only has Thumb)
            list(FILTER _LVGL_SOURCES EXCLUDE REGEX "/blend/helium/")
            list(FILTER _LVGL_SOURCES EXCLUDE REGEX "/blend/neon/")
            # Exclude LVGL's built-in NuttX drivers — oveRTOS uses its
            # own display/input backend, not LVGL's native NuttX drivers
            list(FILTER _LVGL_SOURCES EXCLUDE REGEX "/drivers/nuttx/")

            add_library(ove_lvgl STATIC ${_LVGL_SOURCES})
            target_include_directories(ove_lvgl PRIVATE
                ${BOARD_DIR}
                ${_LVGL_PATH}
                ${OVE_DL_DIR}
                # oveRTOS include (lv_conf.h may include ove/lv_conf_common.h)
                ${OVE_DIR}/include
                # NuttX system headers — LVGL's lv_conf_kconfig.h
                # includes nuttx/config.h when __NuttX__ is defined
                ${CMAKE_BINARY_DIR}/include
                ${CMAKE_BINARY_DIR}/include_arch
            )
            target_compile_definitions(ove_lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)
            target_compile_options(ove_lvgl PRIVATE -w)

            # Register with NuttX so it gets linked into the final binary
            set_property(GLOBAL APPEND PROPERTY NUTTX_EXTRA_LIBRARIES ove_lvgl)

            # Make LVGL headers available to the app
            list(APPEND _OVE_NX_INCLUDE_DIRS
                ${_LVGL_PATH}
                ${OVE_DL_DIR}
                ${BOARD_DIR}
            )
            add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
        endif()
    endif()
endmacro()


# ─── ove_nuttx_build_tflm() ──────────────────────────────────────
# Build TFLM if inference is enabled.  Uses cmake/OveTflm.cmake.
macro(ove_nuttx_build_tflm)
    if(OVE_INFER AND NOT TARGET ove_tflm)
        set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
        set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
        include(${OVE_DIR}/cmake/OveTflm.cmake)
        ove_build_tflm()

        # GCC's C++ headers use #include_next to chain to C headers.
        # Newlib in the default search conflicts with NuttX's libc
        # (div_t redefinition).  Strip defaults, re-add in the order
        # that makes #include_next resolve to NuttX's C headers.
        # Paths pre-computed by 'ove configure' (in ove_config.cmake).
        target_compile_options(ove_tflm PRIVATE -nostdinc
            "$<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>")
        set(_TFLM_PREFIX_H "${CMAKE_CURRENT_BINARY_DIR}/tflm_nuttx_prefix.h")
        file(WRITE "${_TFLM_PREFIX_H}"
            "#include <nuttx/config.h>\n"
            "#include <nuttx/compiler.h>\n")
        target_compile_options(ove_tflm PRIVATE
            "SHELL:-include ${_TFLM_PREFIX_H}"
            "-isystem${OVE_GCC_BUILTIN_INC}")
        foreach(_dir ${OVE_GCC_CXX_DIRS})
            target_compile_options(ove_tflm PRIVATE
                "$<$<COMPILE_LANGUAGE:CXX>:-isystem${_dir}>")
        endforeach()
        target_compile_options(ove_tflm PRIVATE
            "-isystem${CMAKE_BINARY_DIR}/include"
            "-isystem${OVE_NUTTX_INC}"
            "-isystem${OVE_NUTTX_LIBM_INC}"
            "-I${OVE_DIR}/backends/nuttx/include")
        set_property(GLOBAL APPEND PROPERTY NUTTX_EXTRA_LIBRARIES ove_tflm)
    endif()
endmacro()


# ─── ove_nuttx_add_cmsis_dsp() ────────────────────────────────────
# Add CMSIS-DSP sources and includes (for boards like stm32f746g-discovery).
macro(ove_nuttx_add_cmsis_dsp)
    set(_CMSIS_DSP_DIR "${OVE_DL_DIR}/CMSIS-DSP")
    set(_CMSIS_CORE_DIR "${OVE_DL_DIR}/CMSIS_5/CMSIS/Core")

    if(EXISTS "${_CMSIS_DSP_DIR}")
        list(APPEND _OVE_NX_EXTRA_SOURCES
            ${_CMSIS_DSP_DIR}/Source/TransformFunctions/TransformFunctions.c
            ${_CMSIS_DSP_DIR}/Source/ComplexMathFunctions/ComplexMathFunctions.c
            ${_CMSIS_DSP_DIR}/Source/BasicMathFunctions/BasicMathFunctions.c
            ${_CMSIS_DSP_DIR}/Source/CommonTables/CommonTables.c
            ${_CMSIS_DSP_DIR}/Source/SupportFunctions/SupportFunctions.c
        )
        list(APPEND _OVE_NX_INCLUDE_DIRS
            ${_CMSIS_DSP_DIR}/Include
            ${_CMSIS_DSP_DIR}/PrivateInclude
        )
    endif()
    if(EXISTS "${_CMSIS_CORE_DIR}")
        list(APPEND _OVE_NX_INCLUDE_DIRS ${_CMSIS_CORE_DIR}/Include)
    endif()
endmacro()


# ─── ove_nuttx_register_app() ─────────────────────────────────────
# Final step: combine all accumulated sources and register the oveRTOS
# application with NuttX via nuttx_add_application().  Then apply the
# app language layer (C/C++/Rust/Zig) to the resulting target.
macro(ove_nuttx_register_app)
    # Assemble core sources — main.c + backends + board extras
    set(_OVE_NX_ALL_SRCS
        main.c
        ${_OVE_NX_BACKEND_SRC}
        ${_OVE_NX_EXTRA_SOURCES}
    )

    # Build TFLM if enabled
    ove_nuttx_build_tflm()

    # Compile flags
    set(_OVE_NX_CFLAGS "")

    # Register with NuttX
    nuttx_add_application(
        NAME      ${CONFIG_EXTERNAL_OVE_APP_PROGNAME}
        PRIORITY  ${CONFIG_EXTERNAL_OVE_APP_PRIORITY}
        STACKSIZE ${CONFIG_EXTERNAL_OVE_APP_STACKSIZE}
        SRCS      ${_OVE_NX_ALL_SRCS}
        COMPILE_FLAGS ${_OVE_NX_CFLAGS}
        INCLUDE_DIRECTORIES ${_OVE_NX_INCLUDE_DIRS}
    )

    # The target created by nuttx_add_application is apps_<NAME>
    set(_OVE_NX_TARGET "apps_${CONFIG_EXTERNAL_OVE_APP_PROGNAME}")

    # Apply app language support (adds app sources, handles Rust/Zig/models)
    include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)
    ove_apply_app_language(${_OVE_NX_TARGET})

    # Force C++20 for oveRTOS C++ bindings — NuttX defaults to C++17
    # but the bindings use concepts, requires, and std::integral.
    if(OVE_APP_LANG STREQUAL "cpp")
        target_compile_options(${_OVE_NX_TARGET} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>)
    endif()

    # NuttX doesn't link libstdc++, so std::optional/std::array hard-
    # coded calls to __glibcxx_assert_fail are unresolved.  Generate the
    # stub once and add it to whichever target needs C++ (app or TFLM).
    set(_GLIBCXX_COMPAT "${CMAKE_CURRENT_BINARY_DIR}/glibcxx_compat.cpp")
    if(NOT EXISTS "${_GLIBCXX_COMPAT}")
        file(WRITE "${_GLIBCXX_COMPAT}"
            "namespace std {\n"
            "  void __glibcxx_assert_fail(\n"
            "      const char*, int, const char*, const char*) noexcept {\n"
            "    while(1) {}\n"
            "  }\n"
            "}\n")
    endif()
    if(OVE_APP_LANG STREQUAL "cpp" OR TARGET ove_tflm)
        target_sources(${_OVE_NX_TARGET} PRIVATE "${_GLIBCXX_COMPAT}")
    endif()

    # Link LVGL if built
    if(TARGET ove_lvgl)
        target_link_libraries(${_OVE_NX_TARGET} PRIVATE ove_lvgl)
    endif()
    # Link TFLM if built.  TFLite is compiled with -fno-exceptions -fno-rtti
    # so it does not need libstdc++.  NuttX already provides the minimal
    # C++ runtime helpers (operator new, __cxa_pure_virtual, etc.).
    if(TARGET ove_tflm)
        target_link_libraries(${_OVE_NX_TARGET} PRIVATE ove_tflm)
    endif()

    # NuttX link-order fix: Rust/Zig static libraries reference oveRTOS +
    # LVGL symbols. The NuttX linker processes libs left-to-right; ensure
    # the Rust/Zig lib can back-reference LVGL/oveRTOS by re-adding ove_lvgl
    # AFTER the Rust/Zig lib in the link command.
    if(OVE_APP_LANG STREQUAL "rust" OR OVE_APP_LANG STREQUAL "zig")
        if(TARGET ove_lvgl)
            target_link_libraries(${_OVE_NX_TARGET} PRIVATE ove_lvgl)
        endif()
        target_link_options(${_OVE_NX_TARGET} PRIVATE
            "LINKER:--undefined=ove_lvgl_lock")
    endif()

    message(STATUS "oveRTOS NuttX app registered: ${CONFIG_EXTERNAL_OVE_APP_PROGNAME}")
endmacro()
