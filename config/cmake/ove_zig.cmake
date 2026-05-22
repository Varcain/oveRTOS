# oveRTOS Zig CMake Integration
#
# Provides ove_build_zig_lib(TARGET) — builds a Zig staticlib
# and links it into the firmware executable.
#
# Expects the app's CMakeLists.txt to define:
#   APP_ZIG_SRC_DIR  — path to directory containing main.zig
#   APP_ZIG_LIB_NAME — output library name (lib<name>.a)
#
# Uses from ove_config.cmake:
#   OVE_ZIG_TARGET  — e.g. thumb-freestanding-eabihf (optional)
#   OVE_ZIG_PATH    — optional custom zig binary path

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/OveBindingsCommon.cmake)

function(ove_build_zig_lib TARGET)
    if(NOT DEFINED APP_ZIG_SRC_DIR)
        message(FATAL_ERROR "APP_ZIG_SRC_DIR not set by app CMakeLists.txt")
    endif()
    if(NOT DEFINED APP_ZIG_LIB_NAME)
        message(FATAL_ERROR "APP_ZIG_LIB_NAME not set by app CMakeLists.txt")
    endif()

    # ── Resolve zig compiler ──────────────────────────────────────────────
    if(DEFINED OVE_ZIG_PATH AND NOT OVE_ZIG_PATH STREQUAL "")
        set(ZIG_CMD "${OVE_ZIG_PATH}")
    elseif(DEFINED OVE_DIR)
        # Look for downloaded zig in the toolchains directory
        file(GLOB _ZIG_DIRS "${OVE_DIR}/output/toolchains/zig-*")
        foreach(_DIR ${_ZIG_DIRS})
            if(EXISTS "${_DIR}/zig")
                set(ZIG_CMD "${_DIR}/zig")
                break()
            endif()
        endforeach()
        if(NOT DEFINED ZIG_CMD)
            set(ZIG_CMD "zig")
        endif()
    else()
        set(ZIG_CMD "zig")
    endif()


    # ── Determine native vs cross vs WASM build ────────────────────────
    _ove_binding_resolve_target_kind(ZIG_IS_NATIVE ZIG_IS_WASM)

    # ── Output directory ─────────────────────────────────────────────────
    set(ZIG_OUTPUT_DIR "${CMAKE_BINARY_DIR}/zig_output")
    set(ZIG_LIB "${ZIG_OUTPUT_DIR}/lib${APP_ZIG_LIB_NAME}.a")

    # ── Resolve target triple ────────────────────────────────────────────
    set(ZIG_TARGET_ARGS "")
    set(ZIG_CPU_ARGS "")
    if(ZIG_IS_WASM)
        # The pinned Zig (see manifest.yaml: toolchains.zig) has broken
        # emscripten OS support in std: std.posix references emscripten
        # `system` fields that don't exist (IOV_MAX, getrandom, …) and
        # the new 0.16 std.Io.Threaded backend transitively pulls those
        # in even when our code doesn't touch posix directly.  Use
        # wasm32-freestanding instead — our Zig code is a library that
        # emcc links into the final executable, so we only need the
        # wasm32 C ABI, not any Zig std OS services.  C-header
        # discovery still goes through the emscripten sysroot below.
        #
        # The emscripten executable uses -sUSE_PTHREADS which makes
        # wasm-ld link with --shared-memory. Static libraries linked
        # into it must be built with the atomics and bulk-memory
        # features, otherwise wasm-ld rejects them.
        list(APPEND ZIG_TARGET_ARGS "-target" "wasm32-freestanding")
        list(APPEND ZIG_CPU_ARGS
            "-mcpu=baseline+atomics+bulk_memory"
            "-fno-stack-check")
    elseif(ZIG_IS_NATIVE)
        list(APPEND ZIG_TARGET_ARGS "-target" "native-native")
        list(APPEND ZIG_CPU_ARGS "-fPIC" "-fno-stack-check")
    else()
        if(NOT DEFINED OVE_ZIG_TARGET)
            set(OVE_ZIG_TARGET "thumb-freestanding-eabihf")
        endif()
        # Detect float ABI: Zephyr on QEMU uses soft float (nofp)
        if(OVE_ZIG_TARGET MATCHES "eabihf$" AND OVE_RTOS STREQUAL "zephyr")
            # Check if Zephyr is actually using hard float
            if(NOT CONFIG_FPU OR NOT CONFIG_FP_HARDABI)
                string(REGEX REPLACE "eabihf$" "eabi" OVE_ZIG_TARGET "${OVE_ZIG_TARGET}")
            endif()
        endif()
        # NuttX: align with CONFIG_ARCH_FPU (soft float unless the kernel
        # opts in). Mismatched ABIs cause ld "uses VFP register arguments"
        # errors against the soft-float NuttX libraries.
        if(OVE_ZIG_TARGET MATCHES "eabihf$" AND OVE_RTOS STREQUAL "nuttx")
            if(NOT CONFIG_ARCH_FPU)
                string(REGEX REPLACE "eabihf$" "eabi" OVE_ZIG_TARGET "${OVE_ZIG_TARGET}")
            endif()
        endif()
        list(APPEND ZIG_TARGET_ARGS "-target" "${OVE_ZIG_TARGET}")

        # Map MCU to Zig CPU model
        if(DEFINED OVE_MCU)
            string(TOLOWER "${OVE_MCU}" _MCU_LOWER)
            if(_MCU_LOWER MATCHES "stm32f7" OR _MCU_LOWER MATCHES "cortex.m7" OR _MCU_LOWER MATCHES "cmsdk_cm7")
                list(APPEND ZIG_CPU_ARGS "-mcpu=cortex_m7")
            elseif(_MCU_LOWER MATCHES "stm32f4" OR _MCU_LOWER MATCHES "cortex.m4" OR _MCU_LOWER MATCHES "cmsdk_cm4")
                list(APPEND ZIG_CPU_ARGS "-mcpu=cortex_m4")
            elseif(_MCU_LOWER MATCHES "stm32h7")
                list(APPEND ZIG_CPU_ARGS "-mcpu=cortex_m7")
            endif()
        endif()

        # ARM cross-build profile tightening (mirrors the Rust binding's
        # [profile.release] lto=fat / panic=abort posture).  -fno-stack-check
        # removes per-frame `bl __zig_probe_stack` probes that have no
        # meaning on bare-metal Cortex-M; -fsingle-threaded folds out
        # thread-local-state branches in Zig's stdlib (Cortex-M targets
        # are single-core); -fstrip removes symbols from the staticlib
        # output (debuginfo flows via the link map / .elf separately).
        list(APPEND ZIG_CPU_ARGS
            "-fno-stack-check"
            "-fsingle-threaded"
            "-fstrip")

        # Cross-language LTO opt-in (paired with cmake/OveCommon.cmake's
        # -flto=thin on the C side).  Default OFF — declared as a CMake
        # option in OveCommon.cmake; enabling requires a bitcode-aware
        # linker that matches Zig's bundled lld output.
        if(OVE_CROSS_LTO)
            list(APPEND ZIG_CPU_ARGS "-flto")
        endif()
    endif()

    # ── Collect include paths ────────────────────────────────────────────
    set(ZIG_INCLUDE_ARGS "")

    # oveRTOS core headers
    list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/include")
    list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_GEN_DIR}")

    # Backend-specific includes
    if(ZIG_IS_WASM)
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/wasm/include")
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/posix/include")
        # Emscripten sysroot — provides stdio.h, stdlib.h, etc. so that
        # @cImport (which transitively includes ove/log.h → stdio.h) can
        # find the C standard library headers used by the final emcc link.
        if(DEFINED OVE_DL_DIR AND EXISTS "${OVE_DL_DIR}/emsdk")
            set(_EMSDK_SYSROOT "${OVE_DL_DIR}/emsdk/upstream/emscripten/cache/sysroot/include")
            if(EXISTS "${_EMSDK_SYSROOT}")
                list(APPEND ZIG_INCLUDE_ARGS "-I${_EMSDK_SYSROOT}")
            endif()
            set(_EMSDK_SYSTEM "${OVE_DL_DIR}/emsdk/upstream/emscripten/system/include")
            if(EXISTS "${_EMSDK_SYSTEM}")
                list(APPEND ZIG_INCLUDE_ARGS "-I${_EMSDK_SYSTEM}")
            endif()
        endif()
    elseif(OVE_RTOS STREQUAL "posix")
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/posix/include")
    elseif(OVE_RTOS STREQUAL "freertos")
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/freertos/include")
        if(DEFINED FREERTOS_INCLUDE_DIR)
            list(APPEND ZIG_INCLUDE_ARGS "-I${FREERTOS_INCLUDE_DIR}")
        endif()
    elseif(OVE_RTOS STREQUAL "zephyr")
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/zephyr/include")
        if(DEFINED ZEPHYR_BASE)
            # Zephyr headers needed for storage types
            list(APPEND ZIG_INCLUDE_ARGS "-I${ZEPHYR_BASE}/include")
            list(APPEND ZIG_INCLUDE_ARGS "-I${ZEPHYR_BASE}/include/zephyr")
        endif()
    elseif(OVE_RTOS STREQUAL "nuttx")
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_DIR}/backends/nuttx/include")
        # NuttX provides its own libc (stdio.h, stdlib.h, etc.) via
        # its generated include dir. Add as -isystem so it takes priority
        # over the ARM toolchain's newlib headers.
        list(APPEND ZIG_INCLUDE_ARGS "-isystem" "${CMAKE_BINARY_DIR}/include")
        # NuttX's cmake generated config directory for nuttx/config.h
        list(APPEND ZIG_INCLUDE_ARGS "-I${CMAKE_BINARY_DIR}/include")
    endif()

    # Board directory
    _ove_binding_resolve_board_dir(_ZIG_BOARD_DIR)
    if(_ZIG_BOARD_DIR)
        list(APPEND ZIG_INCLUDE_ARGS "-I${_ZIG_BOARD_DIR}")
    endif()

    # LVGL includes — unified via workspace dl/ symlink for all RTOSes
    set(_LVGL_INC "${OVE_DL_DIR}/lvgl")
    set(_LVGL_PARENT "${OVE_DL_DIR}")
    if(EXISTS "${_LVGL_INC}")
        list(APPEND ZIG_INCLUDE_ARGS "-I${_LVGL_INC}")
        list(APPEND ZIG_INCLUDE_ARGS "-I${_LVGL_PARENT}")
    endif()

    # Board lv_conf.h directory — pre-computed by 'ove configure'.
    if(OVE_LV_CONF_DIR)
        list(APPEND ZIG_INCLUDE_ARGS "-I${OVE_LV_CONF_DIR}")
    endif()

    # ARM sysroot include (for cross-compilation: libc headers like stdio.h).
    # NuttX provides its own libc, so skip the ARM newlib sysroot for NuttX
    # to avoid header conflicts (newlib's stdio.h requires types NuttX doesn't
    # define in the same way).
    if(NOT ZIG_IS_NATIVE AND NOT OVE_RTOS STREQUAL "nuttx")
        _ove_binding_arm_sysroot_include(_ARM_SYSROOT)
        if(_ARM_SYSROOT AND EXISTS "${_ARM_SYSROOT}")
            list(APPEND ZIG_INCLUDE_ARGS "-I${_ARM_SYSROOT}")
        endif()
    endif()

    # Native host system includes (Zig's @cImport doesn't inherit GCC paths)
    if(ZIG_IS_NATIVE AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        if(EXISTS "/usr/include")
            list(APPEND ZIG_INCLUDE_ARGS "-I/usr/include")
        endif()
        execute_process(
            COMMAND ${CMAKE_C_COMPILER} -print-multiarch
            OUTPUT_VARIABLE _MULTIARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _MULTIARCH_RES
        )
        if(_MULTIARCH_RES EQUAL 0 AND _MULTIARCH)
            set(_MULTIARCH_INC "/usr/include/${_MULTIARCH}")
            if(EXISTS "${_MULTIARCH_INC}")
                list(APPEND ZIG_INCLUDE_ARGS "-I${_MULTIARCH_INC}")
            endif()
        endif()
    endif()

    # ── Inherit include directories from the CMake target ───────────────
    # The board CMakeLists.txt sets include_directories() globally.
    # Also pick up target-specific includes.
    #
    # Helper: append "-I<dir>" for each entry in a list, expanding any
    # $<TARGET_PROPERTY:<tgt>,INTERFACE_INCLUDE_DIRECTORIES> generator
    # expressions to their constituent paths.  Without this, the gen
    # expression survives until add_custom_command evaluates it, and
    # the result is a single -I argument with semicolon-joined paths
    # which the shell then mis-parses as command separators.
    function(_zig_append_includes outvar)
        set(_acc "${${outvar}}")
        foreach(_INC ${ARGN})
            if(_INC MATCHES "^\\$<TARGET_PROPERTY:([^,>]+),(INTERFACE_)?INCLUDE_DIRECTORIES>$")
                set(_src_target "${CMAKE_MATCH_1}")
                if(TARGET "${_src_target}")
                    get_target_property(_iface_incs "${_src_target}"
                        INTERFACE_INCLUDE_DIRECTORIES)
                    if(_iface_incs)
                        foreach(_iface_inc ${_iface_incs})
                            if(NOT _iface_inc MATCHES "^\\$<")
                                list(APPEND _acc "-I${_iface_inc}")
                            endif()
                        endforeach()
                    endif()
                endif()
            elseif(NOT _INC MATCHES "^\\$<")
                list(APPEND _acc "-I${_INC}")
            endif()
        endforeach()
        set(${outvar} "${_acc}" PARENT_SCOPE)
    endfunction()

    get_target_property(_TARGET_INCS ${TARGET} INCLUDE_DIRECTORIES)
    if(_TARGET_INCS)
        _zig_append_includes(ZIG_INCLUDE_ARGS ${_TARGET_INCS})
    endif()
    get_property(_DIR_INCS DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
    if(_DIR_INCS)
        _zig_append_includes(ZIG_INCLUDE_ARGS ${_DIR_INCS})
    endif()

    # ── Collect defines ──────────────────────────────────────────────────
    set(ZIG_DEFINE_ARGS "")

    # For Zephyr: get defines from zephyr_interface (the canonical source)
    if(OVE_RTOS STREQUAL "zephyr" AND TARGET zephyr_interface)
        get_target_property(_ZEPHYR_DEFS zephyr_interface INTERFACE_COMPILE_DEFINITIONS)
        if(_ZEPHYR_DEFS)
            foreach(_DEF ${_ZEPHYR_DEFS})
                # Skip generator expressions and LV_CONF_PATH (contains
                # quoted path that Zig's cImport can't handle — we use
                # LV_CONF_INCLUDE_SIMPLE + include path instead).
                if(NOT _DEF MATCHES "^\\$<" AND NOT _DEF MATCHES "^LV_CONF_PATH=")
                    list(APPEND ZIG_DEFINE_ARGS "-D${_DEF}")
                elseif(_DEF MATCHES "^LV_CONF_PATH=\"(.+)\"")
                    # Add the directory of lv_conf.h as an include path
                    get_filename_component(_LV_CONF_DIR "${CMAKE_MATCH_1}" DIRECTORY)
                    list(APPEND ZIG_INCLUDE_ARGS "-I${_LV_CONF_DIR}")
                endif()
            endforeach()
        endif()
    endif()

    # Get compile definitions from the target and directory
    get_target_property(_DEFS ${TARGET} COMPILE_DEFINITIONS)
    if(_DEFS)
        foreach(_DEF ${_DEFS})
            if(NOT _DEF STREQUAL "" AND NOT _DEF MATCHES "^\\$<")
                list(APPEND ZIG_DEFINE_ARGS "-D${_DEF}")
            endif()
        endforeach()
    endif()
    get_property(_DIR_DEFS DIRECTORY PROPERTY COMPILE_DEFINITIONS)
    if(_DIR_DEFS)
        foreach(_DEF ${_DIR_DEFS})
            if(NOT _DEF STREQUAL "" AND NOT _DEF MATCHES "^\\$<")
                list(APPEND ZIG_DEFINE_ARGS "-D${_DEF}")
            endif()
        endforeach()
    endif()

    # Ensure LV_CONF_INCLUDE_SIMPLE is defined for @cImport so LVGL finds
    # lv_conf.h via include paths rather than relative #include.
    if(NOT "-DLV_CONF_INCLUDE_SIMPLE" IN_LIST ZIG_DEFINE_ARGS)
        list(APPEND ZIG_DEFINE_ARGS "-DLV_CONF_INCLUDE_SIMPLE")
    endif()

    # ── Generate storage sizes for zero-heap mode ─────────────────────────
    # When CONFIG_OVE_ZERO_HEAP is enabled, Zig's @cImport needs
    # correctly-sized opaque storage types (not 1-byte stubs).
    # Compile a C probe to measure sizeof/alignof, extract KEY=VALUE sizes,
    # then generate a C header that is included via -I before storage.h.
    set(ZIG_SIZES_DEPS "")

    # Read ove_config.h to check for zero-heap mode. Initialize before the
    # conditional read so the if() below is always well-defined, including
    # the first configure pass before ove_config.h has been generated.
    # Storage sizes header is required in BOTH zero-heap and heap modes:
    #   - Zero-heap: caller storage embedded in BSS via `var x: T = undefined;`
    #     needs exact size at compile time.
    #   - Heap: the binding's modernised `Type.create(allocator)` path
    #     embeds storage inline in the wrapper's Backing struct so the Zig
    #     allocator routes a single block.  Without correct sizes,
    #     `c.ove_*_storage_t` is opaque/1-byte, the Backing struct is
    #     undersized, and `ove_*_init` writes its real storage past the
    #     allocation — corrupts the heap.  Failure mode observed: free
    #     list corruption inside pvPortMalloc after the first Queue/
    #     Thread/Timer create.
    set(ZIG_SIZES_C "${CMAKE_BINARY_DIR}/zig_storage_sizes.c")
    set(ZIG_SIZES_ENV "${CMAKE_BINARY_DIR}/zig_storage_sizes.env")
    set(ZIG_SIZES_HDR_DIR "${CMAKE_BINARY_DIR}/zig_sizes_include")
    set(ZIG_SIZES_HDR "${ZIG_SIZES_HDR_DIR}/zig_storage_sizes.h")

    _ove_binding_write_sizes_probe(${ZIG_SIZES_C}
        "#include \"ove/ove.h\"\n#include \"ove/storage.h\"\n")
    _ove_binding_build_sizes_probe(_zig_ove_sizes ${TARGET} ${ZIG_SIZES_C})
    _ove_binding_extract_sizes(${ZIG_SIZES_ENV} _zig_ove_sizes
        "Extracting storage type sizes for Zig bindings")

    # Convert KEY=VALUE .env file into a C header with #define lines.
    # This header is included by @cImport before storage.h via -I path.
    set(ZIG_SIZES_GEN_SCRIPT "${CMAKE_BINARY_DIR}/zig_gen_sizes_hdr.cmake")
    file(WRITE ${ZIG_SIZES_GEN_SCRIPT}
"file(MAKE_DIRECTORY \"${ZIG_SIZES_HDR_DIR}\")\n\
file(READ \"${ZIG_SIZES_ENV}\" _CONTENT)\n\
set(_HDR \"/* Auto-generated storage sizes for Zig builds (heap + zero-heap). */\\n\")\n\
string(REPLACE \"\\n\" \";\" _LINES \"\${_CONTENT}\")\n\
foreach(_LINE \${_LINES})\n\
  if(_LINE MATCHES \"^([A-Z0-9_]+)=([0-9]+)$\")\n\
    string(APPEND _HDR \"#define OVE_\${CMAKE_MATCH_1} \${CMAKE_MATCH_2}\\n\")\n\
  endif()\n\
endforeach()\n\
file(WRITE \"${ZIG_SIZES_HDR}\" \"\${_HDR}\")\n"
    )

    add_custom_command(
        OUTPUT ${ZIG_SIZES_HDR}
        COMMAND ${CMAKE_COMMAND} -P ${ZIG_SIZES_GEN_SCRIPT}
        DEPENDS ${ZIG_SIZES_ENV}
        COMMENT "Generating zig_storage_sizes.h for Zig build"
    )

    # Add the header directory to the include path so @cImport picks it up.
    list(PREPEND ZIG_INCLUDE_ARGS "-I${ZIG_SIZES_HDR_DIR}")
    set(ZIG_SIZES_DEPS ${ZIG_SIZES_HDR})

    # ── oveRTOS Zig bindings path ────────────────────────────────────────
    set(OVE_ZIG_BINDINGS "${OVE_DIR}/bindings/zig/ove")

    # ── Collect Zig source files for dependency tracking ─────────────────
    file(GLOB_RECURSE ZIG_APP_SOURCES CONFIGURE_DEPENDS "${APP_ZIG_SRC_DIR}/*.zig")
    file(GLOB_RECURSE ZIG_OVE_SOURCES CONFIGURE_DEPENDS "${OVE_ZIG_BINDINGS}/src/*.zig")

    # ── Build the Zig library ────────────────────────────────────────────
    # We use `zig build-lib` to compile a static library from the app's
    # main.zig, which imports the ove package.
    #
    # Zig module syntax:
    #   --dep <name>    — add dependency to the NEXT -M module
    #   -M<name>=<src>  — declare a module (first -M is the root/main)
    # -I flags provide C include paths for @cImport resolution.
    # -D flags provide C preprocessor defines for @cImport resolution.
    add_custom_command(
        OUTPUT ${ZIG_LIB}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ZIG_OUTPUT_DIR}
        COMMAND ${ZIG_CMD} build-lib
            ${ZIG_TARGET_ARGS}
            ${ZIG_CPU_ARGS}
            -OReleaseSafe
            -fllvm -flld
            --name ${APP_ZIG_LIB_NAME}
            -femit-bin=${ZIG_LIB}
            --dep ove
            -Mroot=${APP_ZIG_SRC_DIR}/main.zig
            ${ZIG_INCLUDE_ARGS}
            ${ZIG_DEFINE_ARGS}
            -Move=${OVE_ZIG_BINDINGS}/src/root.zig
        WORKING_DIRECTORY ${APP_ZIG_SRC_DIR}
        COMMENT "Building Zig library: ${APP_ZIG_LIB_NAME}"
        DEPENDS ${ZIG_APP_SOURCES}
                ${ZIG_OVE_SOURCES}
                ${ZIG_SIZES_DEPS}
    )

    add_custom_target(zig_lib ALL DEPENDS ${ZIG_LIB})
    add_dependencies(${TARGET} zig_lib)

    _ove_binding_link_lib(${TARGET} ${ZIG_LIB})
endfunction()
