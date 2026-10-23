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

    # oveRTOS overrides (entropy, time, no hosted-OS modules). PUBLIC: every
    # user of the mbedTLS headers must see the same struct layouts.
    target_include_directories(ove_mbedtls PUBLIC ${OVE_DIR}/config/mbedtls)
    target_compile_definitions(ove_mbedtls PUBLIC
        "MBEDTLS_USER_CONFIG_FILE=\"ove_mbedtls_config.h\""
    )

    # Outgoing record size (CONFIG_OVE_NET_TLS_OUT_CONTENT_LEN); without it
    # ove_mbedtls_config.h's 4 KB default applies.
    if(DEFINED OVE_NET_TLS_OUT_CONTENT_LEN)
        target_compile_definitions(ove_mbedtls PUBLIC
            MBEDTLS_SSL_OUT_CONTENT_LEN=${OVE_NET_TLS_OUT_CONTENT_LEN}
        )
    endif()

    set_target_properties(ove_mbedtls PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
    )

    # Suppress third-party warnings
    target_compile_options(ove_mbedtls PRIVATE -w)
    # The bignum multiply-accumulate assembly needs every register; the
    # profiler's frame pointer (OveCommon.cmake) would take r7 from it.
    target_compile_options(ove_mbedtls PRIVATE -fomit-frame-pointer)

    # Zero-heap: use mbedTLS static buffer allocator instead of libc calloc.
    # (MBEDTLS_PLATFORM_C, which both need, is on in mbedTLS's own config;
    # defining it here as well is a redefinition that -Werror rejects.)
    if(OVE_ZERO_HEAP)
        target_compile_definitions(ove_mbedtls PUBLIC
            MBEDTLS_PLATFORM_MEMORY
            MBEDTLS_MEMORY_BUFFER_ALLOC_C
        )
    endif()

    unset(_MBEDTLS_PATH)
    unset(_MBEDTLS_LIB_SRC)
endmacro()
