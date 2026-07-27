# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# oveRTOS Zephyr board CMakeLists.txt for QEMU MPS2-AN521 (Cortex-M33).
#
# A minimal USERSPACE-capable board for the Linux personality: no LVGL/display
# or audio sim — just the kernel, the console, and the oveRTOS backend the app
# selects. When the Linux personality is enabled (CONFIG_OVE_LINUX) it adds the
# one engine-seam link option and the rootfs-fixture include path.

cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/OveZephyrCommon.cmake)

ove_zephyr_pre_find()

# The Linux personality splits PSRAM between a QEMU-loaded rootfs and a NOLOAD
# program-pool linker region (see ove-psram.overlay), so neither costs internal
# flash. Must be set before find_package(Zephyr) (devicetree is processed there).
if(OVE_LINUX)
    set(EXTRA_DTC_OVERLAY_FILE ${CMAKE_CURRENT_LIST_DIR}/ove-psram.overlay)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_firmware)

target_sources(app PRIVATE
    ${OVE_BOARD_DIR}/main.c
    ${OVE_BACKEND_SOURCES}
)

ove_zephyr_include_app_sources()
ove_zephyr_add_profiler_flags()

# The Linux personality seam traps the program's svc by interposing the engine's
# software-fault handler at link time. qemu-run.sh loads the rootfs into PSRAM;
# unlike hardware targets, this board deliberately does not embed the CPIO.
if(OVE_LINUX)
    target_link_options(app PUBLIC "-Wl,--wrap=z_do_kernel_oops")
    ove_zephyr_add_common_includes(EXTRA ${OVE_DIR}/tests/ontarget)
else()
    ove_zephyr_add_common_includes()
endif()
