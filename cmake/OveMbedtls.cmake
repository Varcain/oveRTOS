# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Build mbedTLS as a static library for TLS support.
# Called when CONFIG_OVE_NET_TLS is enabled.

macro(ove_build_mbedtls)
    set(_MBEDTLS_PATH "${OVE_DL_DIR}/mbedtls")

    if(NOT EXISTS "${_MBEDTLS_PATH}")
        message(FATAL_ERROR
            "mbedTLS sources not found at ${_MBEDTLS_PATH}. "
            "Run 'make download' first.")
    endif()

    # Collect mbedTLS library sources
    file(GLOB _MBEDTLS_LIB_SRC CONFIGURE_DEPENDS "${_MBEDTLS_PATH}/library/*.c")

    # Create static library
    add_library(ove_mbedtls STATIC ${_MBEDTLS_LIB_SRC})

    target_include_directories(ove_mbedtls PUBLIC
        ${_MBEDTLS_PATH}/include
    )
    target_include_directories(ove_mbedtls PRIVATE
        ${_MBEDTLS_PATH}/library
    )

    set_target_properties(ove_mbedtls PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
    )

    # Suppress third-party warnings
    target_compile_options(ove_mbedtls PRIVATE -w)

    # Zero-heap: use mbedTLS static buffer allocator instead of libc calloc
    if(OVE_ZERO_HEAP)
        target_compile_definitions(ove_mbedtls PUBLIC
            MBEDTLS_PLATFORM_C
            MBEDTLS_PLATFORM_MEMORY
            MBEDTLS_MEMORY_BUFFER_ALLOC_C
        )
    endif()

    unset(_MBEDTLS_PATH)
    unset(_MBEDTLS_LIB_SRC)
endmacro()
