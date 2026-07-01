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
set(OVE_DIR ${OVE_ROOT})

# This is a standalone `west build` (no `ove build` / ove_config.cmake), so point the fixture
# generator at the Buildroot tree ourselves. Default to the sibling checkout (../buildroot — the
# repo convention, resolved against the oveRTOS root by ove_linux_generate_fixtures); override with
# -DOVE_BUILDROOT=<tree> or the OVE_BUILDROOT env var. OVE_LINUX_ROOTFS_OUTPUT defaults to
# output-fdpic (the FDPIC rootfs) inside the fixture helper, which is what the an521 personality uses.
if(NOT DEFINED OVE_BUILDROOT OR OVE_BUILDROOT STREQUAL "")
    if(DEFINED ENV{OVE_BUILDROOT})
        set(OVE_BUILDROOT "$ENV{OVE_BUILDROOT}")
    else()
        set(OVE_BUILDROOT "../buildroot")
    endif()
endif()

# Generate the rootfs fixture header from the Buildroot image at build time (it
# is not committed); fails clearly if the Buildroot image is unavailable.
include(${OVE_ROOT}/cmake/OveLinuxFixtures.cmake)
ove_linux_generate_fixtures(OVE_LNX_GENDIR FIXTURES rootfs)

# src/main.c plus the engine-agnostic layers the SVC seam dispatches into.
# NOTE: this standalone harness predates the 2026-06-23 run-loop single-sourcing refactor
# (ad213d0) — the modern backends/common/ove_lnx_run.c pulls in the ove_thread stats backend, so
# reviving this build needs the full backend link (zephyr_thread/sync/console/time/queue) + a
# config that selects CONFIG_OVE_RTOS_ZEPHYR. The maintained an521 path is instead
# `ove build qemu-mps2-an521.zephyr.linux_interop`.
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
    ${OVE_LNX_GENDIR}                # generated loader_rootfs_image.h
)

# Interpose the engine's software-fault handler at link time (no Zephyr source
# patch): svc #0 from the unprivileged program reaches z_do_kernel_oops (svc #0
# is "Unused" in Zephyr's dispatch); --wrap routes it to our Linux SVC seam,
# which chains real oopses through to __real_z_do_kernel_oops.
target_link_options(app PUBLIC "-Wl,--wrap=z_do_kernel_oops")
