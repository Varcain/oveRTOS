# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.
#
# Isolated Zephyr CONFIG_USERSPACE variant for the Linux-personality on-target
# bring-up. Kept separate from the full ove_test Zephyr app: the personality
# runs a loaded program as an unprivileged user thread and traps its svc, which
# wants a minimal, dedicated firmware.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ove_test_zephyr_linux)

set(OVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

# src/main.c plus the engine-agnostic layers the SVC seam dispatches into.
target_sources(app PRIVATE
    src/main.c
    ${OVE_ROOT}/backends/zephyr/zephyr_lnx.c
    ${OVE_ROOT}/backends/common/ove_arena.c
    ${OVE_ROOT}/loader/ove_loader.c
    ${OVE_ROOT}/linux/ove_linux_syscall.c
)
target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}      # ove_config.h
    ${OVE_ROOT}/include
    ${OVE_ROOT}/tests/ontarget       # loader_rootfs_image.h (bFLT/rootfs fixtures)
)

# Interpose the engine's software-fault handler at link time (no Zephyr source
# patch): svc #0 from the unprivileged program reaches z_do_kernel_oops (svc #0
# is "Unused" in Zephyr's dispatch); --wrap routes it to our Linux SVC seam,
# which chains real oopses through to __real_z_do_kernel_oops.
target_link_options(app PUBLIC "-Wl,--wrap=z_do_kernel_oops")
