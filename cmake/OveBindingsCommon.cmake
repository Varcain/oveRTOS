# ============================================================================
# OveBindingsCommon.cmake — Shared scaffolding for binding-language integration
# ============================================================================
#
# Helpers used by ove_rust.cmake and ove_zig.cmake. Adding a new binding
# language (AssemblyScript, Carbon, …) should reuse these helpers rather than
# copy/paste the Rust/Zig glue.
#
# Public helpers (single-leading-underscore = "module-internal but reusable
# across binding files"):
#
#   _ove_binding_resolve_board_dir(OUTVAR)
#       Sets OUTVAR to OVE_BOARD_DIR if defined, else BOARD_DIR, else empty.
#
#   _ove_binding_resolve_target_kind(OUT_NATIVE OUT_WASM)
#       Sets OUT_NATIVE / OUT_WASM to TRUE/FALSE based on CMAKE_SYSTEM_NAME
#       and OVE_RTOS. Both can be FALSE (cross-compile to ARM).
#
#   _ove_binding_arm_sysroot_include(OUTVAR)
#       Resolves the ARM toolchain's <triple>/include directory from
#       CMAKE_C_COMPILER. Sets OUTVAR to "" if not resolvable.
#
#   _ove_binding_write_sizes_probe(OUT_C_PATH ROOT_INCLUDES)
#       Writes a C file at OUT_C_PATH containing sizeof/alignof arrays for
#       all conditionally-compiled storage types. ROOT_INCLUDES is the
#       initial #include block (e.g. "#include \"ove/storage.h\"\n").
#
#   _ove_binding_build_sizes_probe(LIB_NAME TARGET)
#       Adds an OBJECT library LIB_NAME compiling a sizes probe and
#       inheriting includes/defines/options from TARGET (or
#       zephyr_interface when applicable).
#
#   _ove_binding_extract_sizes(SIZES_C OUT_ENV LIB_NAME COMMENT)
#       Adds a custom_command that runs extract_storage_sizes.py on
#       LIB_NAME's object output, producing OUT_ENV.
#
#   _ove_binding_link_lib(TARGET LIB)
#       Links a binding-produced static library into TARGET, using the
#       RTOS-specific mechanism (zephyr_link_libraries / NuttX
#       NUTTX_EXTRA_LIBRARIES global / target_link_libraries fallback).

if(_OVE_BINDINGS_COMMON_INCLUDED)
    return()
endif()
set(_OVE_BINDINGS_COMMON_INCLUDED TRUE)


function(_ove_binding_resolve_board_dir OUTVAR)
    if(DEFINED OVE_BOARD_DIR)
        set(${OUTVAR} "${OVE_BOARD_DIR}" PARENT_SCOPE)
    elseif(DEFINED BOARD_DIR)
        set(${OUTVAR} "${BOARD_DIR}" PARENT_SCOPE)
    else()
        set(${OUTVAR} "" PARENT_SCOPE)
    endif()
endfunction()


function(_ove_binding_resolve_target_kind OUT_NATIVE OUT_WASM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set(${OUT_NATIVE} FALSE PARENT_SCOPE)
        set(${OUT_WASM} TRUE PARENT_SCOPE)
    elseif(OVE_RTOS STREQUAL "posix")
        set(${OUT_NATIVE} TRUE PARENT_SCOPE)
        set(${OUT_WASM} FALSE PARENT_SCOPE)
    else()
        set(${OUT_NATIVE} FALSE PARENT_SCOPE)
        set(${OUT_WASM} FALSE PARENT_SCOPE)
    endif()
endfunction()


function(_ove_binding_arm_sysroot_include OUTVAR)
    set(${OUTVAR} "" PARENT_SCOPE)
    if(NOT DEFINED CMAKE_C_COMPILER)
        return()
    endif()
    get_filename_component(_BIN "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_ROOT "${_BIN}" DIRECTORY)
    get_filename_component(_NAME "${CMAKE_C_COMPILER}" NAME)
    string(REGEX REPLACE "-gcc$" "" _TRIPLE "${_NAME}")
    set(${OUTVAR} "${_ROOT}/${_TRIPLE}/include" PARENT_SCOPE)
endfunction()


function(_ove_binding_write_sizes_probe OUT_C_PATH ROOT_INCLUDES)
    file(WRITE ${OUT_C_PATH}
"${ROOT_INCLUDES}\
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
    if(OVE_NET)
        file(APPEND ${OUT_C_PATH}
"S(ove_socket_storage_t)     A(ove_socket_storage_t)\n\
S(ove_netif_storage_t)      A(ove_netif_storage_t)\n"
        )
    endif()
    if(OVE_NET_HTTP)
        file(APPEND ${OUT_C_PATH}
"S(ove_http_client_storage_t) A(ove_http_client_storage_t)\n"
        )
    endif()
    if(OVE_NET_MQTT)
        file(APPEND ${OUT_C_PATH}
"S(ove_mqtt_client_storage_t) A(ove_mqtt_client_storage_t)\n"
        )
    endif()
    if(OVE_NET_TLS)
        file(APPEND ${OUT_C_PATH}
"S(ove_tls_storage_t)        A(ove_tls_storage_t)\n"
        )
    endif()
    if(OVE_INFER)
        file(APPEND ${OUT_C_PATH}
"S(ove_model_storage_t)      A(ove_model_storage_t)\n"
        )
    endif()
    if(OVE_UART)
        file(APPEND ${OUT_C_PATH}
"S(ove_uart_storage_t)       A(ove_uart_storage_t)\n"
        )
    endif()
    if(OVE_SPI)
        file(APPEND ${OUT_C_PATH}
"S(ove_spi_storage_t)        A(ove_spi_storage_t)\n"
        )
    endif()
    if(OVE_I2C)
        file(APPEND ${OUT_C_PATH}
"S(ove_i2c_storage_t)        A(ove_i2c_storage_t)\n"
        )
    endif()
    if(OVE_I2S)
        file(APPEND ${OUT_C_PATH}
"S(ove_i2s_storage_t)        A(ove_i2s_storage_t)\n"
        )
    endif()
endfunction()


function(_ove_binding_build_sizes_probe LIB_NAME TARGET SIZES_C)
    add_library(${LIB_NAME} OBJECT ${SIZES_C})
    if(OVE_RTOS STREQUAL "zephyr" AND TARGET zephyr_interface)
        # Inherit Zephyr's defines/flags AND build-order deps for generated
        # syscall headers. Add oveRTOS includes that aren't in zephyr_interface.
        target_link_libraries(${LIB_NAME} PRIVATE zephyr_interface)
        target_include_directories(${LIB_NAME} PRIVATE
            ${OVE_DIR}/include
            ${OVE_DIR}/backends/zephyr/include
            ${OVE_GEN_DIR}
        )
    else()
        target_include_directories(${LIB_NAME} PRIVATE
            $<TARGET_PROPERTY:${TARGET},INCLUDE_DIRECTORIES>)
        target_compile_definitions(${LIB_NAME} PRIVATE
            $<TARGET_PROPERTY:${TARGET},COMPILE_DEFINITIONS>)
        target_compile_options(${LIB_NAME} PRIVATE
            $<TARGET_PROPERTY:${TARGET},COMPILE_OPTIONS>)
    endif()
    target_compile_options(${LIB_NAME} PRIVATE -w)
endfunction()


function(_ove_binding_extract_sizes OUT_ENV LIB_NAME COMMENT)
    if(NOT EXISTS "${OVE_DIR}/config/scripts/extract_storage_sizes.py")
        message(FATAL_ERROR
            "extract_storage_sizes.py is missing at "
            "${OVE_DIR}/config/scripts/extract_storage_sizes.py")
    endif()
    add_custom_command(
        OUTPUT ${OUT_ENV}
        COMMAND python3
            ${OVE_DIR}/config/scripts/extract_storage_sizes.py
            $<TARGET_OBJECTS:${LIB_NAME}> ${OUT_ENV}
        DEPENDS ${LIB_NAME}
                ${OVE_DIR}/include/ove/storage.h
        COMMENT "${COMMENT}"
    )
endfunction()


function(_ove_binding_link_lib TARGET LIB)
    if(OVE_RTOS STREQUAL "zephyr" AND COMMAND zephyr_link_libraries)
        zephyr_link_libraries(${LIB})
    elseif(OVE_RTOS STREQUAL "nuttx")
        # NuttX link: add to NUTTX_EXTRA_LIBRARIES so it's in the link group.
        set_property(GLOBAL APPEND PROPERTY NUTTX_EXTRA_LIBRARIES ${LIB})
        target_link_libraries(${TARGET} PRIVATE ${LIB})
    else()
        target_link_libraries(${TARGET} PRIVATE ${LIB})
    endif()
endfunction()
