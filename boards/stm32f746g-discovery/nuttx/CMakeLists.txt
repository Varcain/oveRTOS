# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

############################################################################
# boards/stm32f746g-discovery/nuttx/CMakeLists.txt
#
# NuttX CMake application for oveRTOS on STM32F746G-Discovery.
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

# ── STM32 MCU flags ──────────────────────────────────────────────
if(CONFIG_ARCH_FPU)
    add_compile_options(-mfloat-abi=hard -mfpu=fpv5-sp-d16)
endif()

add_compile_definitions(ARM_MATH_CM7 __FPU_PRESENT=1)
add_compile_options(-O3)

# ── CMSIS-DSP library ────────────────────────────────────────────
ove_nuttx_add_cmsis_dsp()

# LVGL
ove_nuttx_build_lvgl()

# Build firmware app
ove_nuttx_register_app()
