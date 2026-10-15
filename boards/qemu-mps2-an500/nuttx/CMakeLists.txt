# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# boards/qemu-mps2-an500/nuttx/CMakeLists.txt
#
# NuttX CMake application for oveRTOS on QEMU MPS2-AN500.
# Sim framework replaces the NuttX display/audio backends — QEMU uses
# semihosting transport, never native drivers.
############################################################################

if(NOT CONFIG_EXTERNAL_OVE_APP)
    return()
endif()

# Resolve OVE_DIR from environment (set by build.py) or .ove_env file
if(DEFINED ENV{OVE_DIR})
    set(OVE_DIR "$ENV{OVE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.ove_env")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/.ove_env" _env_lines)
    foreach(_line ${_env_lines})
        if(_line MATCHES "^OVE_DIR=(.+)$")
            set(OVE_DIR "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

# oveRTOS NuttX integration
include(${OVE_DIR}/cmake/OveNuttX.cmake)
ove_nuttx_setup()

# oveRTOS backend sources (from generated config)
ove_nuttx_add_backend_sources()

# The NuttX backends for console, time, filesystem, NVS and watchdog run
# unchanged on this machine; a module whose device QEMU lacks reports that at
# run time. GPIO is the board's RAM bank (QEMU emulates no AN500 GPIO), and
# qemu_board.c is the BOARD backend.
ove_nuttx_exclude_backends(BOARD GPIO)
ove_nuttx_add_sources(${OVE_DIR}/boards/qemu-mps2-an500/qemu_board.c)
if(OVE_GPIO)
    ove_nuttx_add_sources(${OVE_DIR}/boards/qemu-mps2-an500/qemu_gpio.c)
endif()

if(OVE_LINUX)
    # Linux personality build is headless: no sim display/audio framework. It
    # only needs the embedded rootfs CPIO fixture on the include path.
    ove_nuttx_exclude_backends(LVGL AUDIO)
    include(${OVE_DIR}/cmake/OveLinuxFixtures.cmake)
    ove_linux_generate_fixtures(OVE_LNX_GENDIR FIXTURES rootfs)
    ove_nuttx_add_include_dirs(${OVE_LNX_GENDIR})
else()

# ── Sim framework (replaces NuttX display/audio backends) ────────
set(SIM_DIR "${OVE_DIR}/sim")
set(OVE_BOARD_BASE "${OVE_DIR}/boards/qemu-mps2-an500")
set(QEMU_SRC_DIR "${OVE_BOARD_BASE}/src")

ove_nuttx_exclude_backends(LVGL AUDIO)

set(SIM_QEMU_SOURCES
    ${SIM_DIR}/hal/sim_board.c
    ${SIM_DIR}/hal/sim_display.c
    ${SIM_DIR}/hal/sim_debug.c
    ${SIM_DIR}/hal/sim_trace.c
    ${SIM_DIR}/hal/sim_profiler.c
    ${SIM_DIR}/hal/sim_audio.c
    ${SIM_DIR}/src/ove_sim_registry.c
    ${SIM_DIR}/src/ove_sim_input.c
    ${SIM_DIR}/src/ove_sim_transport_shm_guest.c
)
if(OVE_LVGL)
    list(APPEND SIM_QEMU_SOURCES ${SIM_DIR}/hal/sim_lvgl.c)
endif()
if(OVE_ASYNC_NET)
    list(APPEND SIM_QEMU_SOURCES ${QEMU_SRC_DIR}/qemu_net_async.c)
endif()

ove_nuttx_add_sources(${SIM_QEMU_SOURCES})

ove_nuttx_add_include_dirs(
    ${OVE_DIR}/sim/include
    ${OVE_DIR}/sim/src
    ${QEMU_SRC_DIR}
)

# LVGL
ove_nuttx_build_lvgl()

endif() # OVE_LINUX (sim framework excluded for the headless personality build)

# Build firmware app
ove_nuttx_register_app()
