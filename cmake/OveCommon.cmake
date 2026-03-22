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

    # Application directory
    if(NOT DEFINED OVE_APP_DIR)
        file(STRINGS "${OVE_DIR}/.config" _app_line REGEX "^CONFIG_OVE_APP_NAME=")
        if(_app_line)
            string(REGEX REPLACE ".*=\"(.*)\"" "\\1" _app_name "${_app_line}")
            set(OVE_APP_DIR "${OVE_DIR}/apps/${_app_name}")
        endif()
    endif()

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

    # Default to Release
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()

    # Declare project — include CXX if app language requires it
    if(OVE_APP_LANG STREQUAL "cpp")
        project(${_OVE_PROJ_NAME} C CXX ASM)
    else()
        project(${_OVE_PROJ_NAME} C ASM)
    endif()
    enable_language(ASM)

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

        # Determine if we should add the stub
        set(_add_stub FALSE)
        if("${_MOD_UPPER}" STREQUAL "BSP" OR "${_MOD_UPPER}" STREQUAL "GPIO")
            set(_add_stub TRUE)
        elseif(OVE_${_MOD_UPPER})
            set(_add_stub TRUE)
        endif()

        if(_add_stub)
            # Add stub file only if it exists (some modules like BOARD
            # just need the backend removed, with no stub replacement)
            if(EXISTS "${_stub_dir}/stub_${_mod_lower}.c")
                list(APPEND _OVE_STUB_SOURCES "${_stub_dir}/stub_${_mod_lower}.c")
            endif()

            # stub_time.c needs OVE_QEMU_ARM for ARM-specific tick source
            if("${_MOD_UPPER}" STREQUAL "TIME")
                set_source_files_properties(
                    "${_stub_dir}/stub_time.c" PROPERTIES
                    COMPILE_DEFINITIONS OVE_QEMU_ARM)
            endif()

            # Remove the corresponding real backend from _OVE_BACKEND_SRC.
            # Only remove files under backends/ (not dispatchers under src/).
            # Backend files match: backends/*/<rtos>_<module>.c or backends/*/stm32f7_bsp.c
            set(_new_backend "")
            foreach(_bsrc ${_OVE_BACKEND_SRC})
                get_filename_component(_bname "${_bsrc}" NAME)
                set(_exclude FALSE)
                # Only filter files in the backends/ directory
                if("${_bsrc}" MATCHES "/backends/")
                    # Match <rtos>_<module>.c pattern
                    if("${_bname}" MATCHES "_(${_mod_lower})\\.c$")
                        set(_exclude TRUE)
                    endif()
                    # Special case: BSP backend may be named stm32f7_bsp.c
                    if("${_MOD_UPPER}" STREQUAL "BSP" AND "${_bname}" MATCHES "_bsp\\.c$")
                        set(_exclude TRUE)
                    endif()
                endif()
                if(NOT _exclude)
                    list(APPEND _new_backend "${_bsrc}")
                endif()
            endforeach()
            set(_OVE_BACKEND_SRC ${_new_backend})
        endif()
    endforeach()
endmacro()


# ─── ove_exclude_backends(MOD1 [MOD2 ...]) ──────────────────────────
# Remove backend sources for listed modules WITHOUT adding stubs.
# Use this when the board provides its own implementation (e.g. qemu_board.c).
macro(ove_exclude_backends)
    foreach(_mod ${ARGN})
        string(TOUPPER "${_mod}" _MOD_UPPER)
        string(TOLOWER "${_mod}" _mod_lower)
        set(_new_backend "")
        foreach(_bsrc ${_OVE_BACKEND_SRC})
            get_filename_component(_bname "${_bsrc}" NAME)
            set(_exclude FALSE)
            if("${_bsrc}" MATCHES "/backends/")
                if("${_bname}" MATCHES "_(${_mod_lower})\\.c$")
                    set(_exclude TRUE)
                endif()
                if("${_MOD_UPPER}" STREQUAL "BSP" AND "${_bname}" MATCHES "_bsp\\.c$")
                    set(_exclude TRUE)
                endif()
            endif()
            if(NOT _exclude)
                list(APPEND _new_backend "${_bsrc}")
            endif()
        endforeach()
        set(_OVE_BACKEND_SRC ${_new_backend})
    endforeach()
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


# ─── ove_build_lvgl() ────────────────────────────────────────────────
# Build LVGL as a static library from dl/lvgl/src/*.c.
macro(ove_build_lvgl)
    set(_LVGL_PATH "${OVE_DL_DIR}/lvgl")

    file(GLOB_RECURSE _LVGL_SOURCES "${_LVGL_PATH}/src/*.c")
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


# ─── ove_link_firmware(linker_script) ─────────────────────────────────
# Assemble all sources into the firmware executable, apply app language,
# link libraries, and generate post-build artifacts (hex, bin, size).
macro(ove_link_firmware _linker_script)
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
    # Post-build: size
    add_custom_command(TARGET ${_OVE_PROJ_NAME}.elf POST_BUILD
        COMMAND ${CMAKE_SIZE} ${_OVE_PROJ_NAME}.elf
        COMMENT "Memory usage:"
    )

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
