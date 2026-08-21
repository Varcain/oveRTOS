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

if(OVE_LINUX)
    # Headless Linux personality: skip the LVGL/CMSIS-DSP framework (the guest brings its own
    # LVGL as an FDPIC binary).
    ove_nuttx_exclude_backends(LVGL AUDIO)
    # With the rootfs in QSPI (OVE_LINUX_ROOTFS_QSPI) it is programmed to the NOR separately
    # (flash-qspi.sh), NOT embedded — the 1 MB internal flash can't hold the ~1.45 MB LVGL
    # rootfs. Otherwise embed the build-generated CPIO consumed by the host bootstrap.
    if(NOT OVE_LINUX_ROOTFS_QSPI)
        include(${OVE_DIR}/cmake/OveLinuxFixtures.cmake)
        ove_linux_generate_fixtures(OVE_LNX_GENDIR FIXTURES rootfs)
        ove_nuttx_add_include_dirs(${OVE_DIR}/tests/ontarget ${OVE_LNX_GENDIR})
    else()
        ove_nuttx_add_include_dirs(${OVE_DIR}/tests/ontarget)
    endif()
else()
    # ── CMSIS-DSP library ────────────────────────────────────────────
    ove_nuttx_add_cmsis_dsp()

    # LVGL
    ove_nuttx_build_lvgl()
endif()

# Board source for the personality's QSPI-XIP rootfs + LTDC /dev/fb0 (real-panel LVGL).
# board_init.c provides ove_hal_board_init AND (when OVE_FB) the ove_hal_fb_* backend. Exclude the
# BOARD backend stub so board_init.c is the sole ove_hal_board_init — ove_board_init references it,
# which force-pulls this object (and its strong fb overrides) into the NuttX app archive link (a
# standalone fb source would not be pulled — the weak ove_fb.c stubs would win). nuttx_add_
# application SRCS needs an absolute path (the board dir is copied under nuttx-apps).
if(OVE_QSPI)
    ove_nuttx_exclude_backends(BOARD)
    ove_nuttx_add_sources(${CMAKE_CURRENT_LIST_DIR}/src/board_init.c)
endif()

# Build firmware app
ove_nuttx_register_app()

#############################################################################
# Benchmark apps (live under tests/benchmarks/<lang>/): force -O2 +
# -fno-omit-frame-pointer.  Same audit/hotpath pipeline as FreeRTOS,
# but attached to the `nuttx` final-link target (NuttX's flat-build
# names its ELF `nuttx`, which the CLI later copies to `firmware.elf`
# in images/).  Pass the actual ELF filename as TARGET_FILE so the
# audit scripts read the right artifact.
#############################################################################
if(OVE_APP_DIR MATCHES "/tests/benchmarks/")
    # Compile flags are applied to the apps_<name> archive target;
    # nuttx_add_application created it earlier in this file.
    target_compile_options(${_OVE_NX_TARGET} PRIVATE
        -O2 -fno-omit-frame-pointer)
    target_compile_definitions(${_OVE_NX_TARGET} PRIVATE
        OVE_APP_LANG_NAME=\"${OVE_APP_LANG}\")
    include(${OVE_DIR}/cmake/OveZeroOverheadAudit.cmake)
    set(_audit_target_key "stm32f746_nuttx_benchmark_${OVE_APP_LANG}")
    # Pass ELF_PATH explicitly — the `nuttx` ELF is built by a sibling
    # cmake project (nuttx-cmake) that this directory cannot attach
    # POST_BUILD steps to.  Use a custom target that depends on the
    # final-link artefact so the audit runs after kernel link.
    # CMAKE_BINARY_DIR is the kernel CMake project's root (build/nuttx-cmake/
    # for the production flat-build, plain build/ for the sim test build).
    set(_nuttx_elf "${CMAKE_BINARY_DIR}/nuttx")
    ove_assert_no_dispatch_overhead(${_OVE_NX_TARGET}
        BINDING ${OVE_APP_LANG}
        ELF_PATH ${_nuttx_elf})
    ove_dump_hotpaths(${_OVE_NX_TARGET}
        BINDING ${OVE_APP_LANG}
        TARGET_KEY ${_audit_target_key}
        ELF_PATH ${_nuttx_elf})
endif()
