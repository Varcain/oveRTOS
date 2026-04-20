# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Build lwIP as a static library for FreeRTOS networking.
# Called when CONFIG_OVE_NET is enabled on FreeRTOS backends.

macro(ove_build_lwip)
    set(_LWIP_PATH "${OVE_DL_DIR}/lwip")

    if(NOT EXISTS "${_LWIP_PATH}")
        message(FATAL_ERROR
            "lwIP sources not found at ${_LWIP_PATH}. "
            "Run 'make download' first.")
    endif()

    # lwIP core sources
    file(GLOB _LWIP_CORE_SRC CONFIGURE_DEPENDS
        "${_LWIP_PATH}/src/core/*.c"
        "${_LWIP_PATH}/src/core/ipv4/*.c"
        "${_LWIP_PATH}/src/core/ipv6/*.c"
    )

    # lwIP API sources (socket API, netconn)
    file(GLOB _LWIP_API_SRC CONFIGURE_DEPENDS
        "${_LWIP_PATH}/src/api/*.c"
    )

    # lwIP netif sources (ethernet, SLIP)
    file(GLOB _LWIP_NETIF_SRC CONFIGURE_DEPENDS
        "${_LWIP_PATH}/src/netif/*.c"
    )

    # lwIP FreeRTOS port (sys_arch)
    set(_LWIP_FREERTOS_PORT
        "${OVE_DIR}/backends/freertos/lwip_sys_arch.c"
    )

    set(_ALL_LWIP_SRC
        ${_LWIP_CORE_SRC}
        ${_LWIP_API_SRC}
        ${_LWIP_NETIF_SRC}
        ${_LWIP_FREERTOS_PORT}
    )

    add_library(ove_lwip STATIC ${_ALL_LWIP_SRC})

    target_include_directories(ove_lwip PUBLIC
        ${_LWIP_PATH}/src/include
        ${OVE_DIR}/backends/freertos/lwip_port
    )

    # Board-specific lwipopts.h must be on the include path. BOARD_DIR is
    # set by ove_setup_project(); when ove_lwip is consumed outside a board
    # context (e.g. a future test harness or sanity build), skip these.
    if(DEFINED BOARD_DIR AND EXISTS "${BOARD_DIR}")
        target_include_directories(ove_lwip PUBLIC
            ${BOARD_DIR}/../src
            ${BOARD_DIR}/src
            ${BOARD_DIR}
        )
    endif()

    # FreeRTOS headers needed by sys_arch
    target_include_directories(ove_lwip PRIVATE
        ${_OVE_FREERTOS_INCLUDE_DIRS}
    )

    # oveRTOS headers
    target_include_directories(ove_lwip PRIVATE
        ${OVE_DIR}/include
        ${OVE_GEN_DIR}
    )

    set_target_properties(ove_lwip PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
    )

    # Suppress third-party warnings
    target_compile_options(ove_lwip PRIVATE -w)

    unset(_LWIP_PATH)
    unset(_LWIP_CORE_SRC)
    unset(_LWIP_API_SRC)
    unset(_LWIP_NETIF_SRC)
    unset(_LWIP_FREERTOS_PORT)
    unset(_ALL_LWIP_SRC)
endmacro()
