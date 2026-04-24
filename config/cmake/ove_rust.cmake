# oveRTOS Rust (Cargo) CMake Integration
#
# Provides ove_build_rust_crate(TARGET) — builds a Rust staticlib
# and links it into the firmware executable.
#
# Expects the app's CMakeLists.txt to define:
#   APP_RUST_CRATE_DIR  — path to Cargo.toml directory
#   APP_RUST_LIB_NAME   — crate name (lib<name>.a)
#
# Uses from ove_config.cmake:
#   OVE_RUST_TARGET           — e.g. thumbv7em-none-eabihf
#   OVE_RUST_TOOLCHAIN_PATH   — optional custom cargo/rustc path

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/OveBindingsCommon.cmake)

function(ove_build_rust_crate TARGET)
    if(NOT DEFINED APP_RUST_CRATE_DIR)
        message(FATAL_ERROR "APP_RUST_CRATE_DIR not set by app CMakeLists.txt")
    endif()
    if(NOT DEFINED APP_RUST_LIB_NAME)
        message(FATAL_ERROR "APP_RUST_LIB_NAME not set by app CMakeLists.txt")
    endif()

    # Resolve cargo path
    if(DEFINED OVE_RUST_TOOLCHAIN_PATH AND NOT OVE_RUST_TOOLCHAIN_PATH STREQUAL "")
        set(CARGO_CMD "${OVE_RUST_TOOLCHAIN_PATH}/cargo")
    else()
        set(CARGO_CMD "cargo")
    endif()

    # Defaults (overridden for WASM below)
    set(RUST_WASM_BUILD_STD "")
    set(RUST_NIGHTLY_FLAG "")

    # Determine if this is a native (POSIX), WASM, or cross-compiled build
    _ove_binding_resolve_target_kind(RUST_IS_NATIVE RUST_IS_WASM)
    if(RUST_IS_WASM)
        set(OVE_RUST_TARGET "wasm32-unknown-emscripten")
    endif()

    # Place Rust build artifacts in the CMake build dir, not next to sources
    set(CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/rust_target")

    # Resolve Rust target
    if(RUST_IS_NATIVE)
        # Native build: use host default target (no --target flag needed)
        # Output goes to rust_target/release/ instead of rust_target/<triple>/release/
        set(RUST_LIB_DIR "${CARGO_TARGET_DIR}/release")
        set(RUST_TARGET_ARGS "")
    else()
        if(NOT DEFINED OVE_RUST_TARGET)
            set(OVE_RUST_TARGET "thumbv7em-none-eabihf")
        endif()
        # Align float ABI with the RTOS kernel. Zephyr on QEMU and NuttX
        # with CONFIG_ARCH_FPU disabled both use soft float; linking a
        # hard-float Rust staticlib against them raises ld's "uses VFP
        # register arguments" error.
        if(OVE_RUST_TARGET MATCHES "eabihf$" AND OVE_RTOS STREQUAL "zephyr")
            if(NOT CONFIG_FPU OR NOT CONFIG_FP_HARDABI)
                string(REGEX REPLACE "eabihf$" "eabi" OVE_RUST_TARGET "${OVE_RUST_TARGET}")
            endif()
        endif()
        if(OVE_RUST_TARGET MATCHES "eabihf$" AND OVE_RTOS STREQUAL "nuttx")
            if(NOT CONFIG_ARCH_FPU)
                string(REGEX REPLACE "eabihf$" "eabi" OVE_RUST_TARGET "${OVE_RUST_TARGET}")
            endif()
        endif()
        set(RUST_LIB_DIR "${CARGO_TARGET_DIR}/${OVE_RUST_TARGET}/release")
        set(RUST_TARGET_ARGS "--target;${OVE_RUST_TARGET}")
    endif()

    set(RUST_LIB "${RUST_LIB_DIR}/lib${APP_RUST_LIB_NAME}.a")

    # LVGL include path — unified across all RTOSes via workspace dl/ symlink.
    # Zephyr's bundled LVGL is replaced with a symlink to the same external
    # source during download, so all backends use OVE_DL_DIR/lvgl.
    set(LVGL_INC_PATH "${OVE_DL_DIR}/lvgl")
    set(LVGL_PARENT_PATH "${OVE_DL_DIR}")

    # Resolve board directory (some platforms use BOARD_DIR, others OVE_BOARD_DIR)
    _ove_binding_resolve_board_dir(_BOARD_DIR)

    # Board lv_conf.h directory — pre-computed by 'ove configure'.
    set(LV_CONF_DIR "${OVE_LV_CONF_DIR}")

    # Build environment variables
    set(CARGO_ENV_VARS
        "OVE_DIR=${OVE_DIR}"
        "OVE_GEN_DIR=${OVE_GEN_DIR}"
        "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "LVGL_INCLUDE_PATH=${LVGL_INC_PATH}"
        "LVGL_PARENT_PATH=${LVGL_PARENT_PATH}"
        "LV_CONF_PATH=${LV_CONF_DIR}"
    )

    if(RUST_IS_NATIVE OR RUST_IS_WASM)
        list(APPEND CARGO_ENV_VARS "RUST_IS_NATIVE=1")
        set(RUST_FEATURE_ARGS "--features;std")
    else()
        set(RUST_FEATURE_ARGS "")

        # NuttX generated headers for bindgen (CMake binary dir)
        if(OVE_RTOS STREQUAL "nuttx")
            list(APPEND CARGO_ENV_VARS
                "NUTTX_INCLUDE_DIR=${CMAKE_BINARY_DIR}/include"
            )
        endif()

        # CMSIS-DSP include paths for bindgen (ARM cross-compilation only)
        if(OVE_RTOS STREQUAL "freertos" AND DEFINED OVE_STM32CUBE_PATH)
            set(_CMSIS_DSP_INC "${OVE_STM32CUBE_PATH}/Drivers/CMSIS/DSP/Include")
            set(_CMSIS_CORE_INC "${OVE_STM32CUBE_PATH}/Drivers/CMSIS/Include")
        elseif(OVE_RTOS STREQUAL "zephyr" AND DEFINED ZEPHYR_BASE)
            set(_CMSIS_DSP_INC "${ZEPHYR_BASE}/../modules/lib/cmsis-dsp/Include")
            set(_CMSIS_CORE_INC "${ZEPHYR_BASE}/../modules/hal/cmsis/CMSIS/Core/Include")
        elseif(OVE_RTOS STREQUAL "nuttx")
            set(_CMSIS_DSP_INC "${OVE_DL_DIR}/CMSIS-DSP/Include")
            set(_CMSIS_CORE_INC "${OVE_DL_DIR}/CMSIS_5/CMSIS/Core/Include")
        endif()

        if(DEFINED _CMSIS_DSP_INC AND EXISTS "${_CMSIS_DSP_INC}/arm_math.h")
            list(APPEND CARGO_ENV_VARS
                "CMSIS_DSP_INCLUDE=${_CMSIS_DSP_INC}"
                "CMSIS_CORE_INCLUDE=${_CMSIS_CORE_INC}"
            )
        endif()
    endif()

    if(RUST_IS_WASM)
        # WASM cross-compilation: use emcc as linker
        # Find the Emscripten sysroot for bindgen to use correct headers
        find_program(EMCC_PATH emcc)
        if(EMCC_PATH)
            get_filename_component(_EMCC_DIR "${EMCC_PATH}" DIRECTORY)
            set(_EM_SYSROOT "${_EMCC_DIR}/cache/sysroot/include")
        endif()
        list(APPEND CARGO_ENV_VARS
            "CARGO_TARGET_WASM32_UNKNOWN_EMSCRIPTEN_LINKER=emcc"
            "CC_wasm32_unknown_emscripten=emcc"
            "OVE_WASM_BUILD=1"
            "RUSTFLAGS=-C target-feature=+atomics,+bulk-memory,+mutable-globals"
        )
        # WASM+pthreads requires rebuilding core/alloc with atomics support.
        # This needs nightly Rust for -Zbuild-std.
        set(RUST_WASM_BUILD_STD "-Zbuild-std=std,panic_abort")
        set(RUST_NIGHTLY_FLAG "+nightly")
        if(EXISTS "${_EM_SYSROOT}")
            list(APPEND CARGO_ENV_VARS "EMSCRIPTEN_SYSROOT=${_EM_SYSROOT}")
        endif()
    elseif(NOT RUST_IS_NATIVE)
        # ARM cross-compilation: set linker and ARM sysroot for bindgen
        if(DEFINED CMAKE_C_COMPILER)
            set(RUST_LINKER "${CMAKE_C_COMPILER}")
        else()
            set(RUST_LINKER "arm-none-eabi-gcc")
        endif()

        string(REPLACE "-" "_" RUST_TARGET_UPPER "${OVE_RUST_TARGET}")
        string(TOUPPER "${RUST_TARGET_UPPER}" RUST_TARGET_UPPER)

        list(APPEND CARGO_ENV_VARS
            "CARGO_TARGET_${RUST_TARGET_UPPER}_LINKER=${RUST_LINKER}"
        )

        # Resolve ARM sysroot include path for bindgen
        _ove_binding_arm_sysroot_include(ARM_SYSROOT_INCLUDE)
        if(ARM_SYSROOT_INCLUDE)
            list(APPEND CARGO_ENV_VARS "ARM_SYSROOT_INCLUDE=${ARM_SYSROOT_INCLUDE}")
        endif()
    endif()

    # ── Generate storage type sizes for Rust bindings ──────────────────
    # WASM uses heap mode (not zero-heap), so storage sizes are not
    # needed — Rust uses opaque *mut c_void handles.
    if(RUST_IS_WASM)
        set(SIZES_ENV "")
    else()
        set(SIZES_C "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
        set(SIZES_ENV "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")
        message(STATUS "[ove-rust] Storage size probe configured: ${SIZES_ENV}")
        _ove_binding_write_sizes_probe(${SIZES_C} "#include \"ove/storage.h\"\n")
        _ove_binding_build_sizes_probe(_ove_sizes ${TARGET} ${SIZES_C})
        _ove_binding_extract_sizes(${SIZES_ENV} _ove_sizes
            "Extracting storage type sizes for Rust bindings")
        list(APPEND CARGO_ENV_VARS "OVE_STORAGE_SIZES=${SIZES_ENV}")
    endif()

    # Collect all Rust source files for dependency tracking
    file(GLOB_RECURSE RUST_SOURCES CONFIGURE_DEPENDS "${APP_RUST_CRATE_DIR}/src/*.rs")

    # Collect shared ove crate sources for dependency tracking
    set(OVE_RUST_CRATE_DIR "${OVE_DIR}/bindings/rust/ove")
    file(GLOB_RECURSE OVE_CRATE_SOURCES CONFIGURE_DEPENDS "${OVE_RUST_CRATE_DIR}/src/*.rs")

    # App build.rs is optional (bindgen may live in shared crate)
    set(APP_BUILD_RS_DEP "")
    if(EXISTS "${APP_RUST_CRATE_DIR}/build.rs")
        set(APP_BUILD_RS_DEP "${APP_RUST_CRATE_DIR}/build.rs")
    endif()

    # Build the Rust crate
    add_custom_command(
        OUTPUT ${RUST_LIB}
        COMMAND ${CMAKE_COMMAND} -E env
            ${CARGO_ENV_VARS}
            ${CARGO_CMD} ${RUST_NIGHTLY_FLAG} build
                ${RUST_TARGET_ARGS}
                ${RUST_FEATURE_ARGS}
                ${RUST_WASM_BUILD_STD}
                --release
                --manifest-path ${APP_RUST_CRATE_DIR}/Cargo.toml
        WORKING_DIRECTORY ${APP_RUST_CRATE_DIR}
        COMMENT "Building Rust crate: ${APP_RUST_LIB_NAME}"
        DEPENDS ${APP_RUST_CRATE_DIR}/Cargo.toml
                ${APP_BUILD_RS_DEP}
                ${RUST_SOURCES}
                ${OVE_RUST_CRATE_DIR}/Cargo.toml
                ${OVE_RUST_CRATE_DIR}/build.rs
                ${OVE_CRATE_SOURCES}
                ${SIZES_ENV}
    )

    add_custom_target(rust_crate ALL DEPENDS ${RUST_LIB})
    add_dependencies(${TARGET} rust_crate)

    _ove_binding_link_lib(${TARGET} ${RUST_LIB})

    # Link any native libraries compiled by the Rust build.rs (e.g. model data).
    # Cargo's cc crate places them in the build output directory.
    file(GLOB _RUST_NATIVE_LIBS "${RUST_LIB_DIR}/build/${APP_RUST_LIB_NAME}-*/out/*.a")
    foreach(_NATIVE_LIB ${_RUST_NATIVE_LIBS})
        _ove_binding_link_lib(${TARGET} ${_NATIVE_LIB})
    endforeach()
endfunction()
