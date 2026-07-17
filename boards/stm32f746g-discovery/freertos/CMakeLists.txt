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
# __FPU_PRESENT is spelled 1U, not 1, to match CMSIS exactly. It cannot simply be
# dropped: the CMSIS DSP sources reach core_cm7.h through arm_math.h without the
# device header, so without it they hit core_cm7.h's "Compiler generates FPU
# instructions for a device without an FPU" #error. But stm32f746xx.h (selected
# by STM32F746xx below) also defines it as 1U, and a macro redefinition only
# warns when the replacement lists differ — `1` vs `1U` did, on every TU that
# reached the device header: 119 of this build's 563 warnings. Identical spelling
# satisfies both.
ove_add_compile_definitions(
    __FPU_PRESENT=1U
    STM32F746xx
    USE_HAL_DRIVER
    ARM_MATH_CM7
    $<$<OR:$<BOOL:${OVE_NET}>,$<BOOL:${OVE_ASYNC_NET}>>:HAL_ETH_MODULE_ENABLED>
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
    src/stm32f7/rng.c
    src/lvgl/lv_port_disp.c
    ${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_init.c
)

# bus_msp_init.c provides board-specific HAL_<peripheral>_MspInit overrides
# (UART, SPI, I2C, ETH, SAI/I2S). Each function is guarded by its own
# CONFIG_OVE_* macro inside the file, so always include it whenever any of
# those subsystems is enabled — otherwise the weak HAL defaults take over
# and the peripheral never gets its GPIO/clock configured.
if(OVE_UART OR OVE_SPI OR OVE_I2C OR OVE_NET OR OVE_ASYNC_NET OR OVE_AUDIO)
    ove_add_board_sources(src/bus_msp_init.c)
endif()

# Linux-personality framebuffer backend (/dev/fb0) over the on-board LTDC panel.
if(OVE_FB)
    ove_add_board_sources(src/fb_port.c)
endif()

if(OVE_NET)
    ove_add_board_sources(${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_eth.c)
endif()
if(OVE_ASYNC_NET)
    ove_add_board_sources(${OVE_DIR}/drivers/freertos/stm32f7/stm32f7_eth_async.c)
endif()

# ── oveRTOS backend sources (from generated config) ─────────────────
ove_add_backend_sources()

# ── Linux personality (headless): embed the BusyBox rootfs.cpio fixture ──────
# The personality demo (#include "loader_rootfs_image.h") needs the embedded cpio on the
# include path; the program-region pool lives in external SDRAM (.sdram_bss, bsp.c brings up
# the FMC + makes it executable). Mirrors the QEMU an500 board's OVE_LINUX block.
if(OVE_LINUX)
    # With the rootfs in QSPI (OVE_LINUX_ROOTFS_QSPI) it is programmed to the NOR
    # separately (flash-qspi.sh), NOT embedded in the firmware image — so skip the
    # cpio-embed fixture generation there.
    if(NOT OVE_LINUX_ROOTFS_QSPI)
        include(${OVE_DIR}/cmake/OveLinuxFixtures.cmake)
        ove_linux_generate_fixtures(OVE_LNX_GENDIR FIXTURES rootfs)
        ove_add_include_directories(${OVE_DIR}/tests/ontarget ${OVE_LNX_GENDIR})
    else()
        ove_add_include_directories(${OVE_DIR}/tests/ontarget)
    endif()
endif()

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
# QSPI NOR (N25Q128A) memory-mapped XIP — the rootfs lives here on real hardware.
if(OVE_QSPI)
    ove_add_stm32cube_bsp(
        BOARD STM32746G-Discovery
        FILES stm32746g_discovery_qspi.c
    )
    # stm32746g_discovery_qspi.h does #include "../Components/n25q128a/n25q128a.h".
    # That component is a git submodule ST does NOT ship in the sparse download, so
    # we vendor it (BSD-3-Clause; see stm32cube/README.md) and install our tracked
    # copy into the Cube tree at the relative-include path. Re-copied every configure,
    # so it survives `make clean` / a fresh checkout (the authoritative file lives
    # under boards/, not in the gitignored dl/ tree).
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/stm32cube/n25q128a.h
         DESTINATION ${OVE_STM32CUBE_PATH}/Drivers/BSP/Components/n25q128a)
endif()
ove_add_fatfs(
    BOARD_DISKIO src/stm32f7/fatfs/sd_diskio.c
)
ove_add_cmsis_dsp()

# ── FreeRTOS kernel ────────────────────────────────────────────────
# OVE_LINUX wants the V11 standalone ARM_CM4_MPU port (PMSAv7, runs on the M7) for
# unprivileged Linux-personality isolation; the STM32Cube V10 MPU port is ABI-incompatible
# with configUSE_PICOLIBC_TLS. Non-Linux keeps the fast STM32Cube V10 ARM_CM7 port.
if(OVE_LINUX)
    set(FREERTOS_PATH "${OVE_DL_DIR}/FreeRTOS-Kernel")
    if(NOT OVE_ZERO_HEAP)
        # xTaskCreate() stacks use an independent heap in internal DTCM; the general heap remains
        # in external SDRAM for the personality's much larger non-stack allocations.
        ove_add_board_sources(src/stm32f7/stack_heap.c)
        ove_add_include_directories(${FREERTOS_PATH})
    endif()
    ove_add_freertos_kernel(ARM_CM4_MPU)
else()
    ove_add_freertos_kernel(ARM_CM7/r0p1)
endif()

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
