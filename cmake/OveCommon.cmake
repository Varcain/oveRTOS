# ============================================================================
# OveCommon.cmake — Shared build infrastructure for oveRTOS firmware
# ============================================================================
#
# Scope (what belongs here vs sibling cmake files):
#
#   THIS FILE owns the FIRMWARE-LEVEL CMake build pipeline:
#     - Project bootstrap (paths, includes, generated-config wiring)
#     - Source / include / flag accumulation (board, app, backend, stub)
#     - Vendored library integration: LVGL, TFLM, STM32Cube HAL/BSP,
#       CMSIS-DSP, FatFS, cpu_utils
#     - Final-link orchestration
#
#   What does NOT belong here, and where it does:
#     - RTOS-specific glue  →  cmake/OveNuttX.cmake
#                              cmake/OveZephyrCommon.cmake
#                              (these call into the macros here)
#     - Per-target binding-language application
#                          →  config/cmake/ove_app_lang.cmake
#                              (called after add_executable(); delegates
#                              to ove_rust.cmake / ove_zig.cmake)
#     - Cross-binding scaffolding (sysroot resolution, sizes-probe lib,
#       target-kind detection) shared by Rust and Zig integration
#                          →  cmake/OveBindingsCommon.cmake
#     - Specific binding integration (cargo invocation, zig build glue,
#       bindgen wiring)    →  config/cmake/ove_rust.cmake
#                              config/cmake/ove_zig.cmake
#     - Zero-heap link-time symbol audit
#                          →  cmake/OveZeroHeapAudit.cmake
#     - Model (TFLite) source generation
#                          →  cmake/OveModels.cmake
#
# Usage (in board CMakeLists.txt):
#   cmake_minimum_required(VERSION 3.20)
#   include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveCommon.cmake)
#   ove_setup_project(firmware)
#   ove_add_board_sources(main.c startup.s)
#   ove_add_backend_sources()
#   ove_add_stub_backends(BSP GPIO CONSOLE TIME)
#   ove_build_lvgl()
#   ove_link_firmware(linker_script.ld)

cmake_minimum_required(VERSION 3.20)

include(${CMAKE_CURRENT_LIST_DIR}/OveHelpers.cmake)

# ─── ove_setup_project(name) ─────────────────────────────────────────
# Resolves all paths, includes generated config, declares the CMake project,
# sets common compiler flags, and includes the application source list.
# Must be called first from the board CMakeLists.txt.
macro(ove_setup_project _proj_name)
    set(_OVE_PROJ_NAME ${_proj_name})

    # Board directory = the CMakeLists.txt source directory
    if(NOT DEFINED BOARD_DIR)
        set(BOARD_DIR ${CMAKE_SOURCE_DIR})
    endif()

    # oveRTOS root: passed via -D or derived from board path
    # Board dirs live at boards/<board-name>/<rtos>/
    if(NOT DEFINED OVE_DIR)
        get_filename_component(OVE_DIR "${BOARD_DIR}/../../.." ABSOLUTE)
    endif()

    # Application directory — resolve from app_paths.json (supports
    # both flat and two-level apps/<lang>/<app> layouts).
    include(${OVE_DIR}/cmake/OveAppResolve.cmake)

    # Generated config directory
    if(NOT DEFINED OVE_GEN_DIR)
        set(OVE_GEN_DIR "${OVE_DIR}/output/generated")
    endif()

    # Download directory (workspace-isolated)
    if(NOT DEFINED OVE_DL_DIR)
        message(FATAL_ERROR
            "OVE_DL_DIR must be set by the build system "
            "(e.g. -DOVE_DL_DIR=<workspace>/dl)")
    endif()

    # Include generated CMake config (sets OVE_RTOS, module flags, etc.)
    if(EXISTS "${OVE_GEN_DIR}/ove_config.cmake")
        include(${OVE_GEN_DIR}/ove_config.cmake)
    endif()

    # The ARM ABI is needed by the toolchain before project(), while Kconfig's
    # generated CMake file is included here. Refuse a stale/mixed build rather
    # than linking objects that disagree on VFP argument passing.
    if(OVE_RTOS STREQUAL "freertos" AND
       DEFINED OVE_CONFIG_ARM_FLOAT_ABI)
        if(NOT DEFINED OVE_ARM_FLOAT_ABI)
            message(FATAL_ERROR
                "OVE_ARM_FLOAT_ABI was not supplied to the FreeRTOS toolchain; "
                "run `ove build` or pass "
                "-DOVE_ARM_FLOAT_ABI=${OVE_CONFIG_ARM_FLOAT_ABI}")
        elseif(NOT OVE_ARM_FLOAT_ABI STREQUAL OVE_CONFIG_ARM_FLOAT_ABI)
            message(FATAL_ERROR
                "Float ABI mismatch: toolchain uses '${OVE_ARM_FLOAT_ABI}' "
                "but Kconfig selects '${OVE_CONFIG_ARM_FLOAT_ABI}'. Clean or "
                "reconfigure this workspace; mixed-ABI objects cannot link safely.")
        endif()
    endif()

    # OVE_DEBUG_BUILD → Debug config; otherwise Release.
    if(OVE_DEBUG)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # Declare project — include CXX if app language or ML inference requires it
    if(OVE_APP_LANG STREQUAL "cpp" OR OVE_INFER)
        project(${_OVE_PROJ_NAME} C CXX ASM)
        set(CMAKE_CXX_STANDARD 23)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
    else()
        project(${_OVE_PROJ_NAME} C ASM)
    endif()
    enable_language(ASM)

    # Always export compile_commands.json for clangd / IDE tooling.
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

    # Speed up rebuilds with ccache when available. OVE_NO_CCACHE=1 opts out.
    if(NOT OVE_NO_CCACHE)
        find_program(CCACHE_PROGRAM ccache)
        if(CCACHE_PROGRAM)
            set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE
                         "${CCACHE_PROGRAM}")
            set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK
                         "${CCACHE_PROGRAM}")
        endif()
    endif()

    # Include board toolchain file (if not already loaded via CMAKE_TOOLCHAIN_FILE)
    include(${BOARD_DIR}/cmake/arm-none-eabi.cmake OPTIONAL)

    # LVGL expects lv_conf.h via simple include
    add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

    # Common compiler flags.  C11 (not C99) so backends/freertos/freertos_pm.c
    # can use <stdatomic.h> (`atomic_int`, `atomic_load_explicit`) and have
    # clang-tidy parse it without complaining that `atomic_int *` isn't a
    # pointer to `_Atomic` — under gnu99 the typedef isn't expanded.
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)

    add_compile_options(
        -Wall
        -Wno-missing-braces
        -ffunction-sections
        -fdata-sections
        $<$<CONFIG:Debug>:-g>
        $<$<CONFIG:Debug>:-O0>
        $<$<CONFIG:Release>:-O2>
    )

    # The C++ binding is exception-free and RTTI-free by design: every
    # dtor/move is `noexcept`, error codes flow through as raw `int`
    # with `[[nodiscard]]` discipline, and there are no `dynamic_cast`s
    # anywhere in the public surface.  Disabling both pulls .eh_frame
    # / .gcc_except_table / typeinfo vtables out of the firmware ELF
    # for a 5-30 KB shrink on Cortex-M.  TFLite already does this via
    # OveTflm.cmake; extending it to the whole CXX surface matches the
    # same posture.  Opt out via `-DOVE_CXX_NOEXCEPT_NORTTI=OFF` if a
    # downstream project legitimately needs exceptions or RTTI in C++.
    #
    # DEVELOPER OPTION — intentionally NOT exposed via Kconfig.  Flipping
    # this in `menuconfig` would invite users to disable a size-critical
    # default without reading the rationale above.  Override at build
    # time only:  `cmake -DOVE_CXX_NOEXCEPT_NORTTI=OFF ...`.
    option(OVE_CXX_NOEXCEPT_NORTTI
        "Compile C++ with -fno-exceptions -fno-rtti (smaller firmware)" ON)
    if(OVE_CXX_NOEXCEPT_NORTTI)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
            $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        )
    endif()

    # Cross-language LTO opt-in.  Default OFF — enabling wires
    # `-flto=thin` into the C/C++ side and `-Clinker-plugin-lto` into
    # the Rust side (handled in config/cmake/ove_rust.cmake).  Requires
    # a clang-style linker that understands LLVM bitcode (lld ≥ 17 or
    # gcc with the LLVM gold plugin), which is not the default for
    # arm-none-eabi-gcc.  Per Gale's "three quiet barriers" post: the
    # gain on already-fast hot paths is 1–4 cycles per cross-call;
    # mismatched target-cpu/target-feature between Rust and C will
    # silently kill inlining without warning.  Turn this on only after
    # measuring that the FFI hop dominates a real workload, and
    # validate against `cargo asm` + `objdump -d` that calls actually
    # inline.
    #
    # DEVELOPER OPTION — intentionally NOT exposed via Kconfig.  The
    # tradeoffs above (toolchain plugin requirement, silent inliner
    # kills, modest gain) make this unsafe to toggle without a specific
    # measurement in hand.  Override at build time only:
    #   `cmake -DOVE_CROSS_LTO=ON ...`.
    option(OVE_CROSS_LTO "Enable cross-language LTO between C and Rust" OFF)
    if(OVE_CROSS_LTO)
        message(STATUS "[ove] Cross-language LTO enabled "
                       "(C: -flto=thin, Rust: -Clinker-plugin-lto)")
        add_compile_options($<$<CONFIG:Release>:-flto=thin>)
        add_link_options($<$<CONFIG:Release>:-flto=thin>)
    endif()

    # NOTE on intra-image GCC LTO (to inline the thin ove_* backend wrappers
    # into callers): investigated and found NOT viable on the arm-none-eabi-gcc
    # 15.2 Cortex-M7 target — whole-image `-flto` fails to assemble the
    # FreeRTOS image with "Error: offset out of range" (a PC-relative
    # literal-pool load exceeds range after LTO merges TUs).  The only
    # mitigations (`-mlong-calls`, partition tuning) negate the inlining gain,
    # which the wrapper-vs-native data shows is modest (~hundreds of ns,
    # worst-case-timing only).  Left unimplemented deliberately; revisit if the
    # toolchain/linker gains robust Cortex-M LTO.

    # Sampling profiler: FreeRTOS walks saved-{r7, lr} pairs out of the
    # task stack, which needs the compiler to emit frame pointers. NuttX
    # uses up_backtrace(tcb), which internally relies on the same ARMv7-M
    # convention — keeping -fno-omit-frame-pointer here is insurance for
    # the Cortex-M path and harmless on the POSIX/WASM targets (their
    # profilers use libc backtrace / emscripten_get_callstack instead).
    if(OVE_PROFILER AND (OVE_RTOS STREQUAL "freertos"
                        OR OVE_RTOS STREQUAL "nuttx"))
        add_compile_options(-fno-omit-frame-pointer)
    endif()

    set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -g -x assembler-with-cpp")

    # Application source list (generated from app.yaml by 'ove configure')
    if(EXISTS "${OVE_GEN_DIR}/app_sources.cmake")
        include(${OVE_GEN_DIR}/app_sources.cmake)
    elseif(EXISTS "${OVE_APP_DIR}/CMakeLists.txt")
        message(WARNING "Using legacy app CMakeLists.txt — migrate to app.yaml")
        include(${OVE_APP_DIR}/CMakeLists.txt)
    else()
        message(FATAL_ERROR "No app build description found. Run 'ove configure' first.")
    endif()

    # Backend storage include path (for ove/storage.h → ove_storage_*.h)
    if(OVE_RTOS STREQUAL "freertos")
        include_directories(${OVE_DIR}/backends/freertos/include)
    elseif(OVE_RTOS STREQUAL "zephyr")
        include_directories(${OVE_DIR}/backends/zephyr/include)
    elseif(OVE_RTOS STREQUAL "nuttx")
        include_directories(${OVE_DIR}/backends/nuttx/include)
    elseif(OVE_RTOS STREQUAL "posix")
        include_directories(${OVE_DIR}/backends/posix/include)
    else()
        message(FATAL_ERROR
            "OveCommon: unknown OVE_RTOS '${OVE_RTOS}' (expected freertos|zephyr|nuttx|posix)")
    endif()

    # Common include directories
    include_directories(
        ${BOARD_DIR}
        ${BOARD_DIR}/inc
        ${BOARD_DIR}/../src
        ${OVE_GEN_DIR}
        ${APP_INCLUDE_DIRS}
        ${OVE_DIR}/include
        ${OVE_DIR}/modules/lxp/include
        ${OVE_DIR}/bindings/cpp
        ${OVE_DIR}/backends/common
    )

    # ETL — header-only fixed-capacity containers for C++ apps.  No-op
    # when not vendored or when the app language isn't C++.  Loaded after
    # the main include block so apps can `#include <etl/...>` alongside
    # `#include <ove/...>`.
    if(OVE_APP_LANG STREQUAL "cpp")
        include(${OVE_DIR}/cmake/OveEtl.cmake)
        ove_use_etl()
    endif()

    # Initialize accumulator lists
    set(_OVE_BOARD_SOURCES "")
    set(_OVE_BACKEND_SRC "")
    set(_OVE_FREERTOS_SOURCES "")
    set(_OVE_STUB_SOURCES "")
    set(_OVE_EXTRA_SOURCES "")
    set(_OVE_LINK_LIBS "")
endmacro()


# ─── ove_add_compile_definitions(def1 [def2 ...]) ────────────────────
# Add board-specific compile definitions.
macro(ove_add_compile_definitions)
    add_compile_definitions(${ARGN})
endmacro()


# ─── ove_werror_own_sources(<target> <sources>) ──────────────────────
# Compile the project's own sources with -Werror, leaving vendored ones on the
# plain -Wall policy. Vendored == under ${OVE_DL_DIR} (the workspace's dl/,
# where downloads land) or a build-tree path; everything else is ours.
#
# Applied per source file because the firmware target contains both.
#
# A source counts as vendored if EITHER its literal path or its realpath sits
# under a download root. Both tests are needed: the workspace's dl/<name> is a
# symlink to the shared <ove_dir>/dl/<name>-<hash> (utils.hashed_dir), so
# resolving a vendored source leaves the workspace entirely — testing only the
# literal path misses a direct reference, and testing only the realpath against
# the workspace dl/ misses every symlinked one, which silently puts the whole
# STM32Cube tree on -Werror and fails the build on the vendor's warnings.
function(ove_werror_own_sources _target _sources)
    if(NOT DEFINED OVE_DL_DIR)
        return()
    endif()
    set(_dl_roots "")
    foreach(_root "${OVE_DL_DIR}" "${OVE_DIR}/dl")
        if(IS_DIRECTORY "${_root}")
            get_filename_component(_r "${_root}" REALPATH)
            list(APPEND _dl_roots "${_root}" "${_r}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _dl_roots)

    set(_own "")
    foreach(_src IN LISTS _sources)
        if(NOT EXISTS "${_src}")
            continue()  # generated at build time; classified when it exists
        endif()
        get_filename_component(_src_real "${_src}" REALPATH)
        set(_vendored FALSE)
        foreach(_root IN LISTS _dl_roots)
            string(FIND "${_src_real}" "${_root}/" _a)
            string(FIND "${_src}" "${_root}/" _b)
            if(_a EQUAL 0 OR _b EQUAL 0)
                set(_vendored TRUE)
                break()
            endif()
        endforeach()
        if(NOT _vendored)
            list(APPEND _own "${_src}")
        endif()
    endforeach()
    if(_own)
        set_source_files_properties(${_own} TARGET_DIRECTORY ${_target}
                                    PROPERTIES COMPILE_OPTIONS "-Werror")
        list(LENGTH _own _n)
        message(STATUS "[ove] -Werror on ${_n} project source(s); vendored code on -Wall")
    endif()
endfunction()

# ─── ove_add_compile_options(opt1 [opt2 ...]) ────────────────────────
# Add board-specific compile options.
macro(ove_add_compile_options)
    add_compile_options(${ARGN})
endmacro()


# ─── ove_add_include_directories(dir1 [dir2 ...]) ────────────────────
# Add board-specific include directories.
macro(ove_add_include_directories)
    include_directories(${ARGN})
endmacro()


# ─── ove_add_board_sources(source1 [source2 ...]) ────────────────────
# Register board platform sources. Paths relative to BOARD_DIR unless absolute.
macro(ove_add_board_sources)
    foreach(_src ${ARGN})
        if(IS_ABSOLUTE "${_src}")
            list(APPEND _OVE_BOARD_SOURCES "${_src}")
        else()
            list(APPEND _OVE_BOARD_SOURCES "${BOARD_DIR}/${_src}")
        endif()
    endforeach()
endmacro()


# ─── ove_add_backend_sources() ───────────────────────────────────────
# Add enabled oveRTOS backend sources from generated ove_config.cmake.
macro(ove_add_backend_sources)
    if(DEFINED OVE_BACKEND_SOURCES)
        set(_OVE_BACKEND_SRC ${OVE_BACKEND_SOURCES})
    else()
        message(WARNING
            "OVE_BACKEND_SOURCES not defined — "
            "run 'make configure' or 'ove configure' first")
    endif()
endmacro()


# ─── ove_add_stub_backends(MOD1 [MOD2 ...]) ─────────────────────────
# Add stub backend implementations for the listed modules, and remove the
# corresponding real backend sources from _OVE_BACKEND_SRC.
# BSP and GPIO stubs are always added; others are conditional on config.
macro(ove_add_stub_backends)
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
                list(APPEND _OVE_STUB_SOURCES "${_stub_dir}/stub_${_mod_lower}.c")
            endif()
            if("${_MOD_UPPER}" STREQUAL "TIME")
                set_source_files_properties(
                    "${_stub_dir}/stub_time.c" PROPERTIES
                    COMPILE_DEFINITIONS OVE_QEMU_ARM)
            endif()
            _ove_filter_backend_list(_OVE_BACKEND_SRC ${_mod})
        endif()
    endforeach()
endmacro()


# ─── ove_exclude_backends(MOD1 [MOD2 ...]) ──────────────────────────
# Remove backend sources for listed modules WITHOUT adding stubs.
# Use this when the board provides its own implementation (e.g. qemu_board.c).
macro(ove_exclude_backends)
    _ove_filter_backend_list(_OVE_BACKEND_SRC ${ARGN})
endmacro()


# ─── ove_add_extra_sources(source1 [source2 ...]) ────────────────────
# Add arbitrary additional sources. Paths relative to BOARD_DIR unless absolute.
macro(ove_add_extra_sources)
    foreach(_src ${ARGN})
        if(IS_ABSOLUTE "${_src}")
            list(APPEND _OVE_EXTRA_SOURCES "${_src}")
        else()
            list(APPEND _OVE_EXTRA_SOURCES "${BOARD_DIR}/${_src}")
        endif()
    endforeach()
endmacro()


# ─── ove_add_stm32cube_hal(FAMILY <f7|f4|h7> [STARTUP <mcu>]) ────────
# Add STM32CubeXX HAL driver sources, CMSIS device headers, and the
# system_stm32XXxx.c init file.  Sets OVE_STM32CUBE_PATH as a side-effect
# for subsequent ove_add_stm32cube_bsp / ove_add_fatfs / ove_add_cmsis_dsp.
#
# Arguments:
#   FAMILY <fam>       Required: f7 / f4 / h7 / etc. (lowercase).
#   STARTUP <mcu>      Optional: MCU name for the startup file lookup.
#                      e.g. STARTUP stm32f746xx adds
#                      Source/Templates/gcc/startup_stm32f746xx.s
macro(ove_add_stm32cube_hal)
    cmake_parse_arguments(_OVE_CUBE "" "FAMILY;STARTUP" "" ${ARGN})
    if(NOT _OVE_CUBE_FAMILY)
        message(FATAL_ERROR "ove_add_stm32cube_hal: FAMILY not specified (e.g. FAMILY f7)")
    endif()
    string(TOUPPER "${_OVE_CUBE_FAMILY}" _CUBE_FAM_UPPER)
    set(OVE_STM32CUBE_PATH "${OVE_DL_DIR}/STM32Cube${_CUBE_FAM_UPPER}")
    if(NOT EXISTS "${OVE_STM32CUBE_PATH}")
        message(FATAL_ERROR
            "STM32Cube${_CUBE_FAM_UPPER} not found at ${OVE_STM32CUBE_PATH}. "
            "Run 'make download' first.")
    endif()

    # HAL driver sources (exclude *_template.c)
    file(GLOB _HAL_SOURCES CONFIGURE_DEPENDS
        "${OVE_STM32CUBE_PATH}/Drivers/STM32${_CUBE_FAM_UPPER}xx_HAL_Driver/Src/*.c")
    list(FILTER _HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")
    list(APPEND _OVE_EXTRA_SOURCES ${_HAL_SOURCES})

    # system_stm32XXxx.c (clock init, family-level)
    set(_CMSIS_DEVICE_DIR
        "${OVE_STM32CUBE_PATH}/Drivers/CMSIS/Device/ST/STM32${_CUBE_FAM_UPPER}xx")
    list(APPEND _OVE_EXTRA_SOURCES
        "${_CMSIS_DEVICE_DIR}/Source/Templates/system_stm32${_OVE_CUBE_FAMILY}xx.c")

    # Startup assembly (MCU-specific)
    if(_OVE_CUBE_STARTUP)
        list(APPEND _OVE_EXTRA_SOURCES
            "${_CMSIS_DEVICE_DIR}/Source/Templates/gcc/startup_${_OVE_CUBE_STARTUP}.s")
    endif()

    # Include paths (keep CMSIS/Include — required by Rust bindgen)
    include_directories(
        ${OVE_STM32CUBE_PATH}/Drivers/CMSIS/Include
        ${_CMSIS_DEVICE_DIR}/Include
        ${OVE_STM32CUBE_PATH}/Drivers/STM32${_CUBE_FAM_UPPER}xx_HAL_Driver/Inc
    )

    message(STATUS "  STM32Cube: ${OVE_STM32CUBE_PATH}")
endmacro()


# ─── ove_add_stm32cube_bsp(BOARD <name> [FILES <src>...] [COMPONENTS <c>...]) ──
# Add STM32Cube BSP sources for the given board.  Requires explicit file
# list because a BSP directory typically contains sources for peripherals
# the target doesn't use (camera, audio, qspi, ...), some of which have
# dependencies on sub-components that are not part of COMPONENTS.
#
# Arguments:
#   BOARD <name>              Required: STM32Cube BSP board directory name
#                             (e.g. STM32746G-Discovery).
#   FILES <src>...            BSP sources to compile, relative to the BSP
#                             directory (e.g. stm32746g_discovery.c).
#   COMPONENTS <c>...         BSP component drivers to compile (assumes
#                             each component has a single <name>.c file).
macro(ove_add_stm32cube_bsp)
    cmake_parse_arguments(_OVE_BSP "" "BOARD" "FILES;COMPONENTS" ${ARGN})
    if(NOT OVE_STM32CUBE_PATH)
        message(FATAL_ERROR
            "ove_add_stm32cube_bsp: call ove_add_stm32cube_hal(...) first")
    endif()
    if(NOT _OVE_BSP_BOARD)
        message(FATAL_ERROR "ove_add_stm32cube_bsp: BOARD not specified")
    endif()

    set(_BSP_DIR "${OVE_STM32CUBE_PATH}/Drivers/BSP/${_OVE_BSP_BOARD}")
    include_directories(${_BSP_DIR})

    foreach(_bsp_src ${_OVE_BSP_FILES})
        list(APPEND _OVE_EXTRA_SOURCES "${_BSP_DIR}/${_bsp_src}")
    endforeach()

    foreach(_comp ${_OVE_BSP_COMPONENTS})
        set(_COMP_DIR "${OVE_STM32CUBE_PATH}/Drivers/BSP/Components/${_comp}")
        list(APPEND _OVE_EXTRA_SOURCES "${_COMP_DIR}/${_comp}.c")
        include_directories(${_COMP_DIR})
    endforeach()
endmacro()


# ─── ove_add_cmsis_dsp() ────────────────────────────────────────────
# Add the full CMSIS-DSP library from the STM32Cube bundle.  Must be
# called after ove_add_stm32cube_hal.
macro(ove_add_cmsis_dsp)
    if(NOT OVE_STM32CUBE_PATH)
        message(FATAL_ERROR
            "ove_add_cmsis_dsp: call ove_add_stm32cube_hal(...) first")
    endif()
    set(_DSP_DIR "${OVE_STM32CUBE_PATH}/Drivers/CMSIS/DSP")
    foreach(_sub BasicMathFunctions CommonTables ComplexMathFunctions
                 ControllerFunctions FastMathFunctions FilteringFunctions
                 MatrixFunctions StatisticsFunctions SupportFunctions
                 TransformFunctions)
        file(GLOB _DSP_SRC CONFIGURE_DEPENDS "${_DSP_DIR}/Source/${_sub}/*.c")
        list(APPEND _OVE_EXTRA_SOURCES ${_DSP_SRC})
    endforeach()
    file(GLOB _DSP_ASM CONFIGURE_DEPENDS "${_DSP_DIR}/Source/TransformFunctions/*.S")
    list(APPEND _OVE_EXTRA_SOURCES ${_DSP_ASM})
    include_directories(${_DSP_DIR}/Include)
endmacro()


# ─── ove_add_fatfs([BOARD_DISKIO <path>]) ───────────────────────────
# Add FatFs sources from STM32Cube middleware, plus optional board-supplied
# sd_diskio.c.  Must be called after ove_add_stm32cube_hal.
macro(ove_add_fatfs)
    cmake_parse_arguments(_OVE_FATFS "" "" "BOARD_DISKIO" ${ARGN})
    if(NOT OVE_STM32CUBE_PATH)
        message(FATAL_ERROR "ove_add_fatfs: call ove_add_stm32cube_hal(...) first")
    endif()
    set(_FATFS_DIR "${OVE_STM32CUBE_PATH}/Middlewares/Third_Party/FatFs/src")
    file(GLOB _FATFS_SOURCES CONFIGURE_DEPENDS "${_FATFS_DIR}/*.c")
    list(APPEND _FATFS_SOURCES "${_FATFS_DIR}/option/ccsbcs.c")
    list(APPEND _OVE_EXTRA_SOURCES ${_FATFS_SOURCES})
    include_directories(
        ${_FATFS_DIR}
        ${_FATFS_DIR}/drivers
    )
    foreach(_diskio ${_OVE_FATFS_BOARD_DISKIO})
        if(IS_ABSOLUTE "${_diskio}")
            list(APPEND _OVE_EXTRA_SOURCES "${_diskio}")
        else()
            list(APPEND _OVE_EXTRA_SOURCES "${BOARD_DIR}/${_diskio}")
        endif()
    endforeach()
endmacro()


# ─── ove_add_cpu_utils() ────────────────────────────────────────────
# Add STM32Cube CPU utilities (cpu_utils.c).  Optional helper for boards
# that use runtime CPU load reporting.  Must be called after ove_add_stm32cube_hal.
macro(ove_add_cpu_utils)
    if(NOT OVE_STM32CUBE_PATH)
        message(FATAL_ERROR "ove_add_cpu_utils: call ove_add_stm32cube_hal(...) first")
    endif()
    list(APPEND _OVE_EXTRA_SOURCES "${OVE_STM32CUBE_PATH}/Utilities/CPU/cpu_utils.c")
    include_directories(${OVE_STM32CUBE_PATH}/Utilities/CPU)
endmacro()


# ─── ove_build_lvgl() ────────────────────────────────────────────────
# Build LVGL as a static library from dl/lvgl/src/*.c.
macro(ove_build_lvgl)
    set(_LVGL_PATH "${OVE_DL_DIR}/lvgl")

    file(GLOB_RECURSE _LVGL_SOURCES CONFIGURE_DEPENDS "${_LVGL_PATH}/src/*.c")
    add_library(lvgl ${_LVGL_SOURCES})
    target_include_directories(lvgl PRIVATE
        ${BOARD_DIR}/inc
        ${_LVGL_PATH}
        ${OVE_DL_DIR}
    )
    target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)
    # LVGL generates many warnings under -Wall; suppress them
    target_compile_options(lvgl PRIVATE -w)

    # Add LVGL include paths globally
    include_directories(
        ${_LVGL_PATH}
        ${OVE_DL_DIR}
    )
endmacro()


# ─── ove_build_tflm_if_enabled() ───────────────────────────────────────
# Build TFLM as a static library if CONFIG_OVE_INFER is set.
# Automatically called by ove_link_firmware().  Can also be called
# explicitly if boards need custom CMSIS-NN paths.
macro(ove_build_tflm_if_enabled)
    if(OVE_INFER AND NOT TARGET ove_tflm)
        set(OVE_INCLUDE_DIR ${OVE_DIR}/include)
        set(OVE_BACKENDS_COMMON_DIR ${OVE_DIR}/backends/common)
        include(${OVE_DIR}/cmake/OveTflm.cmake)
        ove_build_tflm()
    endif()
endmacro()


# ─── ove_link_firmware(<linker_script> [PRINT_SIZE_CMAKE <script>]) ──
# Assemble all sources into the firmware executable, apply app language,
# link libraries, and generate post-build artifacts (hex, bin, lst, size).
#
# Arguments:
#   <linker_script>           Positional: path to the .ld file (absolute or
#                             relative to BOARD_DIR).
#   PRINT_SIZE_CMAKE <path>   Optional: cmake -P script that prints a memory
#                             summary (with flash/ram percentages).  When
#                             omitted, the default ${CMAKE_SIZE} command is
#                             used.  Path is relative to BOARD_DIR unless
#                             absolute.
macro(ove_link_firmware)
    cmake_parse_arguments(_OVE_LF "" "PRINT_SIZE_CMAKE" "" ${ARGN})
    list(LENGTH _OVE_LF_UNPARSED_ARGUMENTS _OVE_LF_NUM_POS)
    if(_OVE_LF_NUM_POS LESS 1)
        message(FATAL_ERROR "ove_link_firmware: missing linker script argument")
    endif()
    list(GET _OVE_LF_UNPARSED_ARGUMENTS 0 _linker_script)

    # Build TFLM if ML inference is enabled
    ove_build_tflm_if_enabled()

    # Resolve linker script path
    if(IS_ABSOLUTE "${_linker_script}")
        set(_OVE_LD "${_linker_script}")
    else()
        set(_OVE_LD "${BOARD_DIR}/${_linker_script}")
    endif()

    # Combine all sources
    set(_ALL_SOURCES
        ${_OVE_BOARD_SOURCES}
        ${_OVE_BACKEND_SRC}
        ${_OVE_FREERTOS_SOURCES}
        ${_OVE_STUB_SOURCES}
        ${_OVE_EXTRA_SOURCES}
    )

    # Create executable
    add_executable(${_OVE_PROJ_NAME}.elf ${_ALL_SOURCES})

    # Apply application language support (C / C++ / Rust)
    include(${OVE_DIR}/config/cmake/ove_app_lang.cmake)
    ove_apply_app_language(${_OVE_PROJ_NAME}.elf)

    # ── Warnings are errors in our code, not in vendored code ────────
    # firmware.elf deliberately mixes our sources with vendored ones (STM32Cube
    # HAL/CMSIS/DSP, the FreeRTOS kernel) in one target, so this has to be
    # per-source rather than per-target.
    #
    # The split is the point. STM32CubeF7 emits ~444 -Wstrict-aliasing warnings
    # that are not ours to fix, and a blanket -Werror would force a blanket
    # suppression — which would then hide the same mistake in our own code. Our
    # sources already compile clean under -Wall, so this only pins that.
    #
    # Read the target's SOURCES rather than _ALL_SOURCES: the app is attached by
    # ove_apply_app_language() above, so the local list does not contain it and
    # the app — the code most likely to be edited — would be left unguarded.
    get_target_property(_OVE_TGT_SOURCES ${_OVE_PROJ_NAME}.elf SOURCES)
    ove_werror_own_sources(${_OVE_PROJ_NAME}.elf "${_OVE_TGT_SOURCES}")

    # Linker flags
    target_link_options(${_OVE_PROJ_NAME}.elf PRIVATE
        -T${_OVE_LD}
        -Wl,--gc-sections
        -Wl,-Map=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.map
        -Wl,--undefined=uxTopUsedPriority
    )
    # Relink when the linker script changes — CMake does not treat a -T script as an
    # automatic dependency of the link step (an edit to it would otherwise be a silent no-op).
    set_target_properties(${_OVE_PROJ_NAME}.elf PROPERTIES LINK_DEPENDS ${_OVE_LD})

    # Link LVGL if built, plus libm
    if(TARGET lvgl)
        target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE lvgl)
    endif()
    # Link TFLM if built
    if(TARGET ove_tflm)
        target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE ove_tflm stdc++)
    endif()
    # Link lwIP if built
    if(TARGET ove_lwip)
        target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE ove_lwip)
    endif()
    # Link mbedTLS if built
    if(TARGET ove_mbedtls)
        target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE ove_mbedtls)
    endif()
    target_link_libraries(${_OVE_PROJ_NAME}.elf PRIVATE m ${_OVE_LINK_LIBS})

    # Verify the final ELF, not merely the command line: this catches a stale
    # or externally supplied archive with the wrong ARM calling convention.
    if(OVE_RTOS STREQUAL "freertos" AND DEFINED OVE_ARM_FLOAT_ABI)
        if(NOT CMAKE_READELF)
            message(FATAL_ERROR
                "CMAKE_READELF is required for ARM float-ABI verification")
        endif()
        add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DELF_FILE=$<TARGET_FILE:${_OVE_PROJ_NAME}.elf>
                -DREADELF=${CMAKE_READELF}
                -DEXPECTED_ABI=${OVE_ARM_FLOAT_ABI}
                -P ${OVE_DIR}/cmake/VerifyArmFloatAbi.cmake
            COMMENT "Verifying ARM ${OVE_ARM_FLOAT_ABI} ABI"
        )
    endif()

    # Post-build: hex
    add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex
            ${_OVE_PROJ_NAME}.elf ${_OVE_PROJ_NAME}.hex
        COMMENT "Generating ${_OVE_PROJ_NAME}.hex"
    )
    # Post-build: bin
    add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary
            ${_OVE_PROJ_NAME}.elf ${_OVE_PROJ_NAME}.bin
        COMMENT "Generating ${_OVE_PROJ_NAME}.bin"
    )
    # Post-build: lst (disassembly)
    add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_OBJDUMP} -h -S
            ${_OVE_PROJ_NAME}.elf > ${_OVE_PROJ_NAME}.lst
        COMMENT "Generating ${_OVE_PROJ_NAME}.lst"
    )
    # Post-build: size.  Either a board-supplied memory-usage script, or
    # the plain ${CMAKE_SIZE} summary.
    if(_OVE_LF_PRINT_SIZE_CMAKE)
        if(IS_ABSOLUTE "${_OVE_LF_PRINT_SIZE_CMAKE}")
            set(_OVE_SIZE_SCRIPT "${_OVE_LF_PRINT_SIZE_CMAKE}")
        else()
            set(_OVE_SIZE_SCRIPT "${BOARD_DIR}/${_OVE_LF_PRINT_SIZE_CMAKE}")
        endif()
        # MAP_FILE carries the linker's own evaluated Memory Configuration, so a
        # size script can attribute sections to real regions instead of summing
        # every BSS against one total.
        add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DELF_FILE=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.elf
                -DMAP_FILE=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.map
                -DSIZE_TOOL=${CMAKE_SIZE}
                -DFLASH_SIZE=${OVE_FLASH_SIZE}
                -DRAM_SIZE=${OVE_RAM_SIZE}
                $<$<BOOL:${OVE_MARGIN_WARN_BYTES}>:-DMARGIN_WARN_BYTES=${OVE_MARGIN_WARN_BYTES}>
                $<$<BOOL:${OVE_MARGIN_FAIL_BYTES}>:-DMARGIN_FAIL_BYTES=${OVE_MARGIN_FAIL_BYTES}>
                -P ${_OVE_SIZE_SCRIPT}
            COMMENT "Memory usage:"
        )
    else()
        add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
            COMMAND ${CMAKE_SIZE} ${_OVE_PROJ_NAME}.elf
            COMMENT "Memory usage:"
        )
    endif()

    # Zero-heap link-time audit + wrap: forbid FreeRTOS heap_*.c
    # globals (xHeap/ucHeap/pucAlignedHeap) and route libc malloc
    # through ove_heap_lock.c's __wrap_* trampolines so any malloc
    # after ove_heap_lock() traps via the lock check.
    # Not STRICT: FreeRTOS still uses newlib for printf, so newlib's
    # __malloc_av_ etc. show up.  A future libc migration to picolibc
    # (Zephyr-style) would make STRICT viable here too.
    if(OVE_ZERO_HEAP)
        include(${OVE_DIR}/cmake/OveZeroHeapAudit.cmake)
        ove_apply_zero_heap_wrap(${_OVE_PROJ_NAME}.elf)
        ove_zero_heap_assert_no_kernel_alloc(${_OVE_PROJ_NAME}.elf
            RTOS FREERTOS)
    else()
        # Heap-mode unified-heap policy: wrap libc malloc/free/calloc/
        # realloc to route through pvPortMalloc / vPortFree via
        # backends/freertos/freertos_libc_malloc.c's __wrap_* shims.
        # Same --wrap mechanism the zero-heap path uses, just with a
        # different destination (forward to pvPortMalloc instead of
        # trap-or-forward).
        target_link_options(${_OVE_PROJ_NAME}.elf PRIVATE
            "LINKER:--wrap=malloc"
            "LINKER:--wrap=free"
            "LINKER:--wrap=calloc"
            "LINKER:--wrap=realloc"
        )
    endif()

    # QEMU run target (if qemu-run.sh exists at board level)
    if(EXISTS "${BOARD_DIR}/../qemu-run.sh")
        add_custom_target(run
            COMMAND ${BOARD_DIR}/../qemu-run.sh
                ${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.elf
            DEPENDS ${_OVE_PROJ_NAME}.elf
            COMMENT "Running ${_OVE_PROJ_NAME}.elf in QEMU"
        )
    endif()

    # Print configuration summary
    message(STATUS "oveRTOS ${_OVE_PROJ_NAME} build")
    message(STATUS "  OVE_DIR: ${OVE_DIR}")
    message(STATUS "  BOARD_DIR:   ${BOARD_DIR}")
    message(STATUS "  APP_DIR:     ${OVE_APP_DIR}")
    message(STATUS "  GEN_DIR:     ${OVE_GEN_DIR}")
    message(STATUS "  DL_DIR:      ${OVE_DL_DIR}")
endmacro()
