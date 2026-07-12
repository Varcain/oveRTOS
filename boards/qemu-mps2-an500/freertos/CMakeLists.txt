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
)

# FreeRTOS kernel (standalone, Cortex-M7). The Linux personality runs each program
# UNPRIVILEGED in a per-task MPU region set, so it uses the ARM_CM4_MPU port (V11;
# ARMv7-M PMSAv7, runs unchanged on the CM7). Other an500 builds keep the faster
# non-MPU ARM_CM7 port.
if(OVE_LINUX)
    ove_add_freertos_kernel(ARM_CM4_MPU)
else()
    ove_add_freertos_kernel(ARM_CM7/r0p1)
endif()

# oveRTOS backend sources (from generated config)
ove_add_backend_sources()

# Stub backends for hardware-dependent modules not available on QEMU
ove_add_stub_backends(BSP GPIO CONSOLE TIME WATCHDOG SHELL NVS FS)

# BOARD backend excluded — qemu_board.c provides the implementation directly
ove_exclude_backends(BOARD)

# The Linux personality build is headless (no sim display/audio); it only needs
# the embedded rootfs CPIO fixture on the include path. Everything below is the
# sim framework, used by the normal (non-personality) an500 builds.
if(OVE_LINUX)
    # The rootfs cpio is XIP'd from PSRAM @ 0x60000000, where QEMU's `-device loader` places it
    # at reset (see qemu-run.sh) — it is NOT embedded in the ELF (which would overflow the 4 MB
    # FLASH), so no cpio fixture is generated here. This mirrors the STM32 QSPI-NOR path.
    ove_add_include_directories(${OVE_DIR}/tests/ontarget)
    ove_add_stub_backends(AUDIO)
    # /dev/fb0 framebuffer backend: a RAM RGB565 panel pushed to /dev/shm/ove-fb.
    if(OVE_FB)
        ove_add_board_sources(../src/qemu_fb.c)
    endif()
else()

# ── Sim framework (replaces lv_port_disp.c and qemu_audio.c) ──────
# The sim framework provides unified display/audio/input through the
# SHM guest transport (semihosting I/O to /dev/shm files).
set(OVE_BOARD_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
ove_add_include_directories(
    ${OVE_DIR}/sim/include
    ${OVE_DIR}/sim/src
    ${OVE_BOARD_DIR}/src
)
set(SIM_QEMU_SOURCES
    ${OVE_DIR}/sim/src/ove_sim_registry.c
    ${OVE_DIR}/sim/src/ove_sim_input.c
    ${OVE_DIR}/sim/src/ove_sim_transport_shm_guest.c
    ${OVE_DIR}/sim/hal/sim_board.c
    ${OVE_DIR}/sim/hal/sim_display.c
    ${OVE_DIR}/sim/hal/sim_debug.c
    ${OVE_DIR}/sim/hal/sim_trace.c
    ${OVE_DIR}/sim/hal/sim_profiler.c
)
if(OVE_LVGL)
    list(APPEND SIM_QEMU_SOURCES ${OVE_DIR}/sim/hal/sim_lvgl.c)
    ove_exclude_backends(LVGL)
endif()
if(OVE_AUDIO)
    list(APPEND SIM_QEMU_SOURCES ${OVE_BOARD_DIR}/src/qemu_audio.c)
    ove_exclude_backends(AUDIO)
else()
    ove_add_stub_backends(AUDIO)
endif()
ove_add_board_sources(${SIM_QEMU_SOURCES})

endif() # OVE_LINUX (sim framework excluded for the headless personality build)

# lwIP for networking
if(OVE_NET)
    ove_add_board_sources(../src/qemu_net.c)
    include(${OVE_DIR}/cmake/OveLwIP.cmake)
    ove_build_lwip()
endif()
# embassy-net SHM transport (mutually exclusive with OVE_NET).
if(OVE_ASYNC_NET)
    ove_add_board_sources(../src/qemu_net_async.c)
endif()
if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

# ML inference (TensorFlow Lite Micro)
ove_build_tflm_if_enabled()

# LVGL library
ove_build_lvgl()

# Build firmware executable
ove_link_firmware(mps2_an500.ld)
