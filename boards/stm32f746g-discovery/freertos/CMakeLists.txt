# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

cmake_minimum_required(VERSION 3.20)

# ============================================================================
# STM32F746G-Discovery FreeRTOS Build
# ============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveCommon.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveFreeRTOS.cmake)

ove_setup_project(firmware)

# ── Board-specific compile options ───────────────────────────────────
ove_add_compile_definitions(
    __FPU_PRESENT=1
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    $<$<BOOL:${OVE_NET}>:HAL_ETH_MODULE_ENABLED>
)

ove_add_compile_options(
    -ffast-math
    -fsingle-precision-constant
)

# ── Board-specific include directories ──────────────────────────────
# OveCommon already adds BOARD_DIR, BOARD_DIR/inc, and OVE_DIR/include paths.
ove_add_include_directories(
    ${BOARD_DIR}/inc/lvgl
    ${BOARD_DIR}/inc/stm32f7
    ${BOARD_DIR}/inc/stm32f7/fatfs
    ${OVE_DIR}/drivers/freertos/stm32f7
)

# ── Board platform sources ──────────────────────────────────────────
ove_add_board_sources(
    main.c
    src/FreeRTOS-openocd.c
    src/serial_wrapper.c
    src/stm32f7/syscalls.c
    src/stm32f7/bsp.c
    src/lvgl/lv_port_disp.c
    ${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_init.c
)

if(OVE_AUDIO)
    ove_add_board_sources(src/bus_msp_init.c)
endif()

if(OVE_NET)
    ove_add_board_sources(${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_eth.c)
endif()

# ── oveRTOS backend sources (from generated config) ─────────────────
ove_add_backend_sources()

# ── STM32CubeF7 HAL + BSP + FatFs + CMSIS-DSP ───────────────────────
ove_add_stm32cube_hal(FAMILY f7 STARTUP stm32f746xx)
ove_add_stm32cube_bsp(
    BOARD STM32746G-Discovery
    FILES
        stm32746g_discovery.c
        stm32746g_discovery_sd.c
        stm32746g_discovery_lcd.c
        stm32746g_discovery_sdram.c
    COMPONENTS wm8994
)
ove_add_fatfs(
    BOARD_DISKIO src/stm32f7/fatfs/sd_diskio.c
)
ove_add_cmsis_dsp()
# cpu_utils.c provides StartIdleMonitor/EndIdleMonitor referenced by
# the generated FreeRTOSConfig.h via traceTASK_SWITCHED_* macros.
ove_add_cpu_utils()

# ── FreeRTOS kernel (Cortex-M7 port from STM32CubeF7 bundle) ────────
ove_add_freertos_kernel(ARM_CM7/r0p1)

# ── Optional networking + TLS ───────────────────────────────────────
if(OVE_NET)
    include(${OVE_DIR}/cmake/OveLwIP.cmake)
    ove_build_lwip()
endif()
if(OVE_NET_TLS)
    include(${OVE_DIR}/cmake/OveMbedtls.cmake)
    ove_build_mbedtls()
endif()

# ── LVGL library ────────────────────────────────────────────────────
ove_build_lvgl()

# ── Build firmware executable + post-build + memory report ──────────
ove_link_firmware(
    STM32F746NGHx_FLASH.ld
    PRINT_SIZE_CMAKE cmake/print_size.cmake
)

#############################################################################
# Benchmark apps (live under tests/benchmarks/<lang>/): force -O2 +
# -fno-omit-frame-pointer.  STM32F746/FreeRTOS inherits OVE_DEBUG_BUILD=y
# from the board defconfig (-> -g -O0); under -O0 the binding wrappers
# don't inline, so neither the static audit nor the runtime measurements
# would reflect the README's "zero overhead" claim.  Frame pointers stay
# on for the sim profiler's backtrace walker and the FreeRTOS-openocd
# integration.
# Audit + hotpath disassembly run as POST_BUILD on firmware.elf.
#############################################################################
if(OVE_APP_DIR MATCHES "/tests/benchmarks/")
    target_compile_options(firmware.elf PRIVATE -O2 -fno-omit-frame-pointer)
    target_compile_definitions(firmware.elf PRIVATE
        OVE_APP_LANG_NAME=\"${OVE_APP_LANG}\")
    include(${OVE_DIR}/cmake/OveZeroOverheadAudit.cmake)
    if(OVE_ZERO_HEAP)
        set(_audit_target_key "stm32f746_freertos_benchmark_zeroheap_${OVE_APP_LANG}")
    else()
        set(_audit_target_key "stm32f746_freertos_benchmark_${OVE_APP_LANG}")
    endif()
    ove_assert_no_dispatch_overhead(firmware.elf
        BINDING ${OVE_APP_LANG})
    ove_dump_hotpaths(firmware.elf
        BINDING ${OVE_APP_LANG}
        TARGET_KEY ${_audit_target_key})
endif()
