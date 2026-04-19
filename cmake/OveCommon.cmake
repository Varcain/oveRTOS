# ============================================================================
# OveCommon.cmake — Shared build infrastructure for oveRTOS firmware
# ============================================================================
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

    # OVE_DEBUG_BUILD → Debug config; otherwise Release.
    if(OVE_DEBUG)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    elseif(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # Declare project — include CXX if app language or ML inference requires it
    if(OVE_APP_LANG STREQUAL "cpp" OR OVE_INFER)
        project(${_OVE_PROJ_NAME} C CXX ASM)
        set(CMAKE_CXX_STANDARD 17)
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

    # Common compiler flags
    set(CMAKE_C_STANDARD 99)
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
    endif()

    # Common include directories
    include_directories(
        ${BOARD_DIR}
        ${BOARD_DIR}/inc
        ${BOARD_DIR}/../src
        ${OVE_GEN_DIR}
        ${APP_INCLUDE_DIRS}
        ${OVE_DIR}/include
        ${OVE_DIR}/bindings/cpp
        ${OVE_DIR}/backends/common
    )

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

    # Linker flags
    target_link_options(${_OVE_PROJ_NAME}.elf PRIVATE
        -T${_OVE_LD}
        -Wl,--gc-sections
        -Wl,-Map=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.map
        -Wl,--undefined=uxTopUsedPriority
    )

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
        add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DELF_FILE=${CMAKE_BINARY_DIR}/${_OVE_PROJ_NAME}.elf
                -DSIZE_TOOL=${CMAKE_SIZE}
                -DFLASH_SIZE=${OVE_FLASH_SIZE}
                -DRAM_SIZE=${OVE_RAM_SIZE}
                -P ${_OVE_SIZE_SCRIPT}
            COMMENT "Memory usage:"
        )
    else()
        add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
            COMMAND ${CMAKE_SIZE} ${_OVE_PROJ_NAME}.elf
            COMMENT "Memory usage:"
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
