# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# C example app: an RTOS kernel thread and a Linux program running side by side
# (RTOS <-> Linux-personality interop). This is a STANDALONE Zephyr USERSPACE
# app (its own CMakeLists/prj.conf, not an app.yaml framework app) because the
# Linux personality needs mps2/an521 (Cortex-M33 + MPU + CONFIG_USERSPACE); the
# framework's QEMU board (mps2-an500, Cortex-M3) has no MPU and cannot host it.
# See README.md for the build/run command.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_linux_interop)

set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# The demo (src/app.c) + the oveRTOS Zephyr backend primitives it uses
# (ove_thread / ove_queue / ove_time) + the Linux personality seam + the
# engine-agnostic layers the seam dispatches into.
target_sources(app PRIVATE
    src/app.c
    ${OVE_ROOT}/backends/zephyr/zephyr_thread.c
    ${OVE_ROOT}/backends/zephyr/zephyr_queue.c
    ${OVE_ROOT}/backends/zephyr/zephyr_time.c
    ${OVE_ROOT}/backends/zephyr/zephyr_lnx.c
    ${OVE_ROOT}/backends/common/ove_arena.c
    ${OVE_ROOT}/loader/ove_loader.c
    ${OVE_ROOT}/linux/ove_linux_syscall.c
)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}      # ove_config.h
    ${OVE_ROOT}/include
    ${OVE_ROOT}/backends/common      # ove_backend_common.h
    ${OVE_ROOT}/backends/zephyr/include  # ove_storage_zephyr.h (real storage sizes)
    ${OVE_ROOT}/tests/ontarget       # loader_rootfs_image.h (the embedded Buildroot rootfs)
)

# The seam traps the program's svc by interposing the engine's software-fault
# handler at link time (no Zephyr source patch); svc #0 is "Unused" in Zephyr's
# dispatch, so it reaches z_do_kernel_oops, and --wrap routes it to the seam.
target_link_options(app PUBLIC "-Wl,--wrap=z_do_kernel_oops")
