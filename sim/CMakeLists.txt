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
    ${SIM_DIR}/src/ove_sim_input.c
    ${SIM_DIR}/src/ove_sim_transport_shm_local.c
    ${SIM_DIR}/src/ove_sim_transport_shm.c
    ${SIM_DIR}/hal/sim_board.c
)
set(SIM_REPLACED_POSIX_SOURCES posix_console.c)

# Display plugin (replaces posix_lvgl.c)
if(OVE_LVGL)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_lvgl.c
        ${SIM_DIR}/hal/sim_display.c
    )
    list(APPEND SIM_REPLACED_POSIX_SOURCES posix_lvgl.c)
endif()

# Audio plugin (replaces posix_audio.c)
if(OVE_AUDIO)
    list(APPEND SIM_SOURCES
        ${SIM_DIR}/hal/sim_audio.c
    )
    list(APPEND SIM_REPLACED_POSIX_SOURCES posix_audio.c)
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

# Console simulation (replaces posix_console.c — adds dashboard input)
list(APPEND SIM_SOURCES
    ${SIM_DIR}/hal/sim_console.c
)

# Debug plugin (thread + memory snapshots for dashboard)
list(APPEND SIM_SOURCES
    ${SIM_DIR}/hal/sim_debug.c
)

# Trace plugin (thread-state swimlane)
list(APPEND SIM_SOURCES
    ${SIM_DIR}/hal/sim_trace.c
)

# Profiler plugin (sampling profiler + flame graph)
list(APPEND SIM_SOURCES
    ${SIM_DIR}/hal/sim_profiler.c
)
