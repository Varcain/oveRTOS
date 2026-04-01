# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# QEMU MPS2-AN500 FreeRTOS Build
# ============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveCommon.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveFreeRTOS.cmake)

ove_setup_project(firmware)

# Board platform sources
ove_add_board_sources(
    main.c
    startup_CMSDK_CM7.s
    system_CMSDK_CM7.c
    ../qemu_board.c
    ../src/lv_port_disp.c
)

# FreeRTOS kernel (standalone, Cortex-M7 port)
ove_add_freertos_kernel(ARM_CM7/r0p1)

# oveRTOS backend sources (from generated config)
ove_add_backend_sources()

# Stub backends for hardware-dependent modules not available on QEMU
ove_add_stub_backends(BSP GPIO CONSOLE TIME WATCHDOG SHELL NVS FS)

# BOARD backend excluded — qemu_board.c provides the implementation directly
ove_exclude_backends(BOARD)

# Audio: use QEMU semihosting driver when enabled, stub otherwise
if(OVE_AUDIO)
    ove_exclude_backends(AUDIO)
    ove_add_board_sources(../src/qemu_audio.c)
else()
    ove_add_stub_backends(AUDIO)
endif()

# lwIP for networking
if(OVE_NET)
    ove_add_board_sources(../src/qemu_net.c)
    include(${OVE_DIR}/cmake/OveLwIP.cmake)
    ove_build_lwip()
endif()
if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

# LVGL library
ove_build_lvgl()

# Build firmware executable
ove_link_firmware(mps2_an500.ld)
