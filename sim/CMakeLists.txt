# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

# Simulation plugin framework sources.
# Included by the board CMakeLists.txt when OVE_SIM is enabled.

set(SIM_DIR ${CMAKE_CURRENT_LIST_DIR})

set(SIM_INCLUDE_DIRS
    ${SIM_DIR}/include
    ${SIM_DIR}/src
)

# Core framework
set(SIM_SOURCES
    ${SIM_DIR}/src/ove_sim_registry.c
    ${SIM_DIR}/src/ove_sim_transport_direct.c
    ${SIM_DIR}/src/ove_sim_ws.c
    ${SIM_DIR}/hal/sim_board.c
)

# Display plugin (replaces posix_lvgl.c)
if(OVE_LVGL)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_lvgl.c
        ${SIM_DIR}/hal/sim_display.c
    )
endif()

# Audio plugin (replaces posix_audio.c)
if(OVE_AUDIO)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_audio.c
    )
endif()

# Network simulation (replaces posix_net.c and ove_net_tls.c)
if(OVE_NET)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_net.c
    )
endif()
if(OVE_NET_TLS)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_tls.c
    )
endif()

# PM simulation (replaces posix_pm.c)
if(OVE_PM)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_pm.c
    )
endif()

# Watchdog simulation (replaces posix_watchdog.c)
if(OVE_WATCHDOG)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_watchdog.c
    )
endif()

# Export to parent scope
set(SIM_SOURCES ${SIM_SOURCES} PARENT_SCOPE)
set(SIM_INCLUDE_DIRS ${SIM_INCLUDE_DIRS} PARENT_SCOPE)

# POSIX HAL files that the sim framework REPLACES.
# The board CMakeLists.txt should exclude these when OVE_SIM is on.
set(SIM_REPLACED_POSIX_SOURCES "" PARENT_SCOPE)
if(OVE_LVGL)
    list(APPEND SIM_REPLACED_POSIX_SOURCES
        posix_lvgl.c
    )
endif()
if(OVE_AUDIO)
    list(APPEND SIM_REPLACED_POSIX_SOURCES
        posix_audio.c
    )
endif()
set(SIM_REPLACED_POSIX_SOURCES ${SIM_REPLACED_POSIX_SOURCES} PARENT_SCOPE)
