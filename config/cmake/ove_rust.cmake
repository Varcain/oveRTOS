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

    # Determine if this is a native (POSIX) or cross-compiled build
    if(OVE_RTOS STREQUAL "posix")
        set(RUST_IS_NATIVE TRUE)
    else()
        set(RUST_IS_NATIVE FALSE)
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
        set(RUST_LIB_DIR "${CARGO_TARGET_DIR}/${OVE_RUST_TARGET}/release")
        set(RUST_TARGET_ARGS "--target;${OVE_RUST_TARGET}")
    endif()

    set(RUST_LIB "${RUST_LIB_DIR}/lib${APP_RUST_LIB_NAME}.a")

    # Determine LVGL include path per RTOS
    if(OVE_RTOS STREQUAL "zephyr" AND DEFINED ZEPHYR_BASE)
        set(LVGL_INC_PATH "${ZEPHYR_BASE}/../modules/lib/gui/lvgl")
        set(LVGL_PARENT_PATH "${ZEPHYR_BASE}/../modules/lib/gui")
    else()
        set(LVGL_INC_PATH "${OVE_DL_DIR}/lvgl")
        set(LVGL_PARENT_PATH "${OVE_DL_DIR}")
    endif()

    # Resolve board directory (some platforms use BOARD_DIR, others OVE_BOARD_DIR)
    if(DEFINED OVE_BOARD_DIR)
        set(_BOARD_DIR "${OVE_BOARD_DIR}")
    elseif(DEFINED BOARD_DIR)
        set(_BOARD_DIR "${BOARD_DIR}")
    endif()

    # Board-specific lv_conf.h directory for bindgen
    if(OVE_RTOS STREQUAL "posix")
        set(LV_CONF_DIR "${OVE_DIR}/boards/host-pc/posix")
    elseif(OVE_RTOS STREQUAL "freertos")
        set(LV_CONF_DIR "${_BOARD_DIR}/inc")
    elseif(OVE_RTOS STREQUAL "zephyr")
        set(LV_CONF_DIR "${OVE_BOARD_DIR}")
    elseif(OVE_RTOS STREQUAL "nuttx")
        set(LV_CONF_DIR "${_BOARD_DIR}/inc")
    endif()

    # Build environment variables
    set(CARGO_ENV_VARS
        "OVE_DIR=${OVE_DIR}"
        "OVE_GEN_DIR=${OVE_GEN_DIR}"
        "CARGO_TARGET_DIR=${CARGO_TARGET_DIR}"
        "LVGL_INCLUDE_PATH=${LVGL_INC_PATH}"
        "LVGL_PARENT_PATH=${LVGL_PARENT_PATH}"
        "LV_CONF_PATH=${LV_CONF_DIR}"
    )

    if(RUST_IS_NATIVE)
        list(APPEND CARGO_ENV_VARS "RUST_IS_NATIVE=1")
        set(RUST_FEATURE_ARGS "--features;std")
    else()
        set(RUST_FEATURE_ARGS "")

        # CMSIS-DSP include paths for bindgen (ARM cross-compilation only)
        if(OVE_RTOS STREQUAL "freertos" AND DEFINED STM32CUBE_PATH)
            set(_CMSIS_DSP_INC "${STM32CUBE_PATH}/Drivers/CMSIS/DSP/Include")
            set(_CMSIS_CORE_INC "${STM32CUBE_PATH}/Drivers/CMSIS/Include")
        elseif(OVE_RTOS STREQUAL "zephyr" AND DEFINED ZEPHYR_BASE)
            set(_CMSIS_DSP_INC "${ZEPHYR_BASE}/../modules/lib/cmsis-dsp/Include")
            set(_CMSIS_CORE_INC "${ZEPHYR_BASE}/../modules/hal/cmsis/CMSIS/Core/Include")
        endif()

        if(DEFINED _CMSIS_DSP_INC AND EXISTS "${_CMSIS_DSP_INC}/arm_math.h")
            list(APPEND CARGO_ENV_VARS
                "CMSIS_DSP_INCLUDE=${_CMSIS_DSP_INC}"
                "CMSIS_CORE_INCLUDE=${_CMSIS_CORE_INC}"
            )
        endif()
    endif()

    if(NOT RUST_IS_NATIVE)
        # Cross-compilation: set linker and ARM sysroot for bindgen
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
        # Detect triple from compiler name (works for arm-none-eabi-gcc, arm-zephyr-eabi-gcc, etc.)
        get_filename_component(TOOLCHAIN_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
        get_filename_component(TOOLCHAIN_ROOT "${TOOLCHAIN_BIN_DIR}" DIRECTORY)
        get_filename_component(_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
        string(REGEX REPLACE "-gcc$" "" _ARM_TRIPLE "${_COMPILER_NAME}")
        set(ARM_SYSROOT_INCLUDE "${TOOLCHAIN_ROOT}/${_ARM_TRIPLE}/include")
        list(APPEND CARGO_ENV_VARS "ARM_SYSROOT_INCLUDE=${ARM_SYSROOT_INCLUDE}")
    endif()

    # ── Generate storage type sizes for Rust bindings ──────────────────
    # Create a tiny object library that compiles a C file with
    # sizeof/alignof arrays using the same flags as the main target.
    # Then extract sizes from the .o and pass them to cargo.
    set(SIZES_C "${CMAKE_BINARY_DIR}/ove_storage_sizes.c")
    set(SIZES_ENV "${CMAKE_BINARY_DIR}/ove_storage_sizes.env")

    message(STATUS "[ove-rust] Storage size probe configured: ${SIZES_ENV}")
    if(NOT EXISTS "${OVE_DIR}/config/scripts/extract_storage_sizes.py")
        message(FATAL_ERROR "[ove-rust] extract_storage_sizes.py is missing at ${OVE_DIR}/config/scripts/extract_storage_sizes.py")
    endif()

    file(WRITE ${SIZES_C}
"#include \"ove/storage.h\"\n\
#include <stddef.h>\n\
#define S(type) unsigned char _sizeof_##type[sizeof(type)];\n\
#define A(type) unsigned char _alignof_##type[_Alignof(type)];\n\
S(ove_thread_storage_t)     A(ove_thread_storage_t)\n\
S(ove_queue_storage_t)      A(ove_queue_storage_t)\n\
S(ove_timer_storage_t)      A(ove_timer_storage_t)\n\
S(ove_mutex_storage_t)      A(ove_mutex_storage_t)\n\
S(ove_sem_storage_t)        A(ove_sem_storage_t)\n\
S(ove_event_storage_t)      A(ove_event_storage_t)\n\
S(ove_condvar_storage_t)    A(ove_condvar_storage_t)\n\
S(ove_eventgroup_storage_t) A(ove_eventgroup_storage_t)\n\
S(ove_workqueue_storage_t)  A(ove_workqueue_storage_t)\n\
S(ove_work_storage_t)       A(ove_work_storage_t)\n\
S(ove_stream_storage_t)     A(ove_stream_storage_t)\n\
S(ove_watchdog_storage_t)   A(ove_watchdog_storage_t)\n\
S(ove_file_storage_t)       A(ove_file_storage_t)\n\
S(ove_dir_storage_t)        A(ove_dir_storage_t)\n"
    )
    # Conditionally add networking storage types
    if(OVE_NET)
        file(APPEND ${SIZES_C}
"S(ove_socket_storage_t)     A(ove_socket_storage_t)\n\
S(ove_netif_storage_t)      A(ove_netif_storage_t)\n"
        )
    endif()
    if(OVE_NET_HTTP)
        file(APPEND ${SIZES_C}
"S(ove_http_client_storage_t) A(ove_http_client_storage_t)\n"
        )
    endif()
    if(OVE_NET_MQTT)
        file(APPEND ${SIZES_C}
"S(ove_mqtt_client_storage_t) A(ove_mqtt_client_storage_t)\n"
        )
    endif()
    if(OVE_NET_TLS)
        file(APPEND ${SIZES_C}
"S(ove_tls_storage_t)        A(ove_tls_storage_t)\n"
        )
    endif()
    if(OVE_INFER)
        file(APPEND ${SIZES_C}
"S(ove_model_storage_t)      A(ove_model_storage_t)\n"
        )
    endif()

    # Build the sizes probe as an OBJECT library that inherits all
    # compile settings from the main target.
    add_library(_ove_sizes OBJECT ${SIZES_C})

    if(OVE_RTOS STREQUAL "zephyr" AND TARGET zephyr_interface)
        # Zephyr: link against zephyr_interface for Zephyr includes,
        # defines, flags, AND build-order deps (generated syscall headers).
        # Also add oveRTOS's own includes which aren't in zephyr_interface.
        target_link_libraries(_ove_sizes PRIVATE zephyr_interface)
        target_include_directories(_ove_sizes PRIVATE
            ${OVE_DIR}/include
            ${OVE_DIR}/backends/zephyr/include
            ${OVE_GEN_DIR}
        )
    else()
        target_include_directories(_ove_sizes PRIVATE
            $<TARGET_PROPERTY:${TARGET},INCLUDE_DIRECTORIES>)
        target_compile_definitions(_ove_sizes PRIVATE
            $<TARGET_PROPERTY:${TARGET},COMPILE_DEFINITIONS>)
        target_compile_options(_ove_sizes PRIVATE
            $<TARGET_PROPERTY:${TARGET},COMPILE_OPTIONS>)
    endif()
    target_compile_options(_ove_sizes PRIVATE -w)

    # Extract sizes from the compiled object after it's built
    add_custom_command(
        OUTPUT ${SIZES_ENV}
        COMMAND python3
            ${OVE_DIR}/config/scripts/extract_storage_sizes.py
            $<TARGET_OBJECTS:_ove_sizes> ${SIZES_ENV}
        DEPENDS _ove_sizes
                ${OVE_DIR}/include/ove/storage.h
        COMMENT "Extracting storage type sizes for Rust bindings"
    )

    list(APPEND CARGO_ENV_VARS "OVE_STORAGE_SIZES=${SIZES_ENV}")

    # Collect all Rust source files for dependency tracking
    file(GLOB_RECURSE RUST_SOURCES "${APP_RUST_CRATE_DIR}/src/*.rs")

    # Collect shared ove crate sources for dependency tracking
    set(OVE_RUST_CRATE_DIR "${OVE_DIR}/bindings/rust/ove")
    file(GLOB_RECURSE OVE_CRATE_SOURCES "${OVE_RUST_CRATE_DIR}/src/*.rs")

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
            ${CARGO_CMD} build
                ${RUST_TARGET_ARGS}
                ${RUST_FEATURE_ARGS}
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

    # Zephyr uses a special link mechanism; standard target_link_libraries on
    # the 'app' target does not propagate to the final executable.
    if(OVE_RTOS STREQUAL "zephyr" AND COMMAND zephyr_link_libraries)
        zephyr_link_libraries(${RUST_LIB})
    else()
        target_link_libraries(${TARGET} PRIVATE ${RUST_LIB})
    endif()

    # Link any native libraries compiled by the Rust build.rs (e.g. model data).
    # Cargo's cc crate places them in the build output directory.
    file(GLOB _RUST_NATIVE_LIBS "${RUST_LIB_DIR}/build/${APP_RUST_LIB_NAME}-*/out/*.a")
    foreach(_NATIVE_LIB ${_RUST_NATIVE_LIBS})
        if(OVE_RTOS STREQUAL "zephyr" AND COMMAND zephyr_link_libraries)
            zephyr_link_libraries(${_NATIVE_LIB})
        else()
            target_link_libraries(${TARGET} PRIVATE ${_NATIVE_LIB})
        endif()
    endforeach()
endfunction()
