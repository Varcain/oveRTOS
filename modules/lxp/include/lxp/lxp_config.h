/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of the lxp module (the OS-agnostic Linux personality).
 *
 * Compile-time configuration: feature gates + sizing/placement knobs. Replaces
 * the module's former dependence on oveRTOS's generated ove_config.h and its
 * CONFIG_OVE_RTOS_* branches. A consumer sets these three ways (highest wins):
 *
 *   1. -DLXP_ENABLE_NET=1 … on the compiler command line (the oveRTOS build maps
 *      its Kconfig to these; a standalone CMake exposes them as option()s).
 *   2. A drop-in "lxp_config_user.h" anywhere on the include path (picked up
 *      automatically below).
 *   3. Otherwise the safe defaults here apply — smallest footprint, so a missing
 *      knob degrades capacity, never correctness.
 */

#ifndef LXP_CONFIG_H
#define LXP_CONFIG_H

#if defined(__has_include)
#if __has_include("lxp_config_user.h")
#include "lxp_config_user.h"
#endif
#endif

/* ---- feature gates (were CONFIG_OVE_LINUX* / _LOADER / _FT5336) ------------- */
#ifndef LXP_ENABLE_LOADER
#define LXP_ENABLE_LOADER 1 /* the FDPIC loader is the core; on by default */
#endif
#ifndef LXP_ENABLE_NET
#define LXP_ENABLE_NET 0
#endif
#ifndef LXP_ENABLE_NETFS
#define LXP_ENABLE_NETFS 0
#endif
#ifndef LXP_ENABLE_NETFS_EXEC
#define LXP_ENABLE_NETFS_EXEC 0
#endif
#ifndef LXP_ENABLE_PTY
#define LXP_ENABLE_PTY 0
#endif
#ifndef LXP_ENABLE_DEV
#define LXP_ENABLE_DEV 0
#endif
#ifndef LXP_ENABLE_DEV_FB
#define LXP_ENABLE_DEV_FB 0
#endif
#ifndef LXP_ENABLE_DEV_INPUT
#define LXP_ENABLE_DEV_INPUT 0
#endif
#ifndef LXP_ENABLE_DEV_INPUT_TESTPAD
#define LXP_ENABLE_DEV_INPUT_TESTPAD 0
#endif
#ifndef LXP_ENABLE_TOUCH
#define LXP_ENABLE_TOUCH 0 /* was CONFIG_OVE_FT5336 (the touch controller hook) */
#endif

/* ---- sizing / placement knobs (were the CONFIG_OVE_RTOS_* #if blocks) ------ */
/* Per-process program region: a dynamic FDPIC proc XIPs its text from the rootfs,
 * so the region holds only the main exec's RW + ld.so RW + stack. 256K is the
 * default (Zephyr used 512K for roomy PSRAM). */
#ifndef LXP_PROG_REGION_SIZE
#define LXP_PROG_REGION_SIZE 0x40000u /* 256K */
#endif
#ifndef LXP_PROG_ARENA_SIZE
#define LXP_PROG_ARENA_SIZE 0x18000u /* 96K program heap */
#endif
/* A dynamic proc's shared arena: every loaded .so's RW segment + brk/mmap heap. */
#ifndef LXP_DYN_POOL_SIZE
#define LXP_DYN_POOL_SIZE 0x80000u /* 512K */
#endif
/* Max program images live at once (init + login shell + concurrent jobs). */
#ifndef LXP_NREG
#define LXP_NREG 8
#endif
/* NREG + transient vfork-window slots. */
#ifndef LXP_NSLOT
#define LXP_NSLOT (LXP_NREG + 4)
#endif
/* Pipe pool: count + per-pipe ring size. The STM32F746 consumer bumps NPIPE to
 * 12 (SSH pipelines) and relocates the pool via LXP_FAR_BSS; Zephyr shrinks the
 * ring to 2048. Defaults are the safe minimum. */
#ifndef LXP_NPIPE
#define LXP_NPIPE 4
#endif
#ifndef LXP_PIPE_BUF
#define LXP_PIPE_BUF 4096
#endif
/* PTY line buffer. Zephyr used 512; default 1024. */
#ifndef LXP_PTY_BUF
#define LXP_PTY_BUF 1024
#endif

/* Section attribute for large "far" pools (the pipe ring on the STM32 lives in
 * external SDRAM). Empty default => ordinary .bss. The STM32 consumer defines
 * this to __attribute__((section(".sdram_bss"))). */
#ifndef LXP_FAR_BSS
#define LXP_FAR_BSS
#endif

#endif /* LXP_CONFIG_H */
