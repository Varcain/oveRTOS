/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Private generated-memory contract shared by the oveRTOS LXP backends.
 */

#ifndef LXP_OVE_MEMORY_LAYOUT_H
#define LXP_OVE_MEMORY_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include "ove_config.h"

#include "board_desc.h"

#if !defined(CONFIG_OVE_LINUX)
#error "lxp_ove_memory_layout.h is only valid for Linux-personality builds"
#endif

#if !defined(CONFIG_OVE_LINUX_ROOTFS_BASE) || !defined(CONFIG_OVE_LINUX_ROOTFS_SIZE)
#error "Linux-personality rootfs layout was not generated"
#endif

#define OVE_LXP_ROOTFS_BASE ((uintptr_t)CONFIG_OVE_LINUX_ROOTFS_BASE)
#define OVE_LXP_ROOTFS_SIZE ((uintptr_t)CONFIG_OVE_LINUX_ROOTFS_SIZE)
#define OVE_LXP_ROOTFS_END (OVE_LXP_ROOTFS_BASE + OVE_LXP_ROOTFS_SIZE)

#if defined(CONFIG_OVE_LINUX_GUEST_POOL_BASE) && CONFIG_OVE_LINUX_GUEST_POOL_BASE != 0
#define OVE_LXP_GUEST_POOL_BASE ((uintptr_t)CONFIG_OVE_LINUX_GUEST_POOL_BASE)
#define OVE_LXP_GUEST_POOL_SIZE ((uintptr_t)CONFIG_OVE_LINUX_GUEST_POOL_SIZE)
#define OVE_LXP_GUEST_POOL_END (OVE_LXP_GUEST_POOL_BASE + OVE_LXP_GUEST_POOL_SIZE)
#endif

/* Every supported layout covers the rootfs with one PMSAv7 window, so its size
 * must be a power of two. */
#define OVE_LXP_ROOTFS_MPU0_BASE OVE_LXP_ROOTFS_BASE
#define OVE_LXP_ROOTFS_MPU0_SIZE OVE_LXP_ROOTFS_SIZE

#if defined(OVE_MEMORY_SDRAM_START) && defined(OVE_MEMORY_SDRAM_KB)
/* External SDRAM, from the board description. */
#define OVE_LXP_SDRAM_BASE ((uintptr_t)OVE_MEMORY_SDRAM_START)
#define OVE_LXP_SDRAM_SIZE ((size_t)OVE_MEMORY_SDRAM_KB * 1024u)
#define OVE_LXP_SDRAM_END (OVE_LXP_SDRAM_BASE + OVE_LXP_SDRAM_SIZE)

#if defined(OVE_DISPLAY_WIDTH) && defined(OVE_DISPLAY_HEIGHT)
/* The display framebuffer: RGB565 at the bottom of SDRAM. */
#define OVE_LXP_FRAMEBUFFER_BASE OVE_LXP_SDRAM_BASE
#define OVE_LXP_FRAMEBUFFER_SIZE ((size_t)OVE_DISPLAY_WIDTH * OVE_DISPLAY_HEIGHT * 2u)
#endif
#endif

/* Where the NuttX port's guests may execute from (its MPU code region: the
 * board's flash) and the window every native task control block must lie in
 * (its on-chip RAM). */
#if defined(OVE_MEMORY_FLASH_START) && defined(OVE_MEMORY_FLASH_KB)
#define OVE_LXP_CODE_BASE ((uintptr_t)OVE_MEMORY_FLASH_START)
#define OVE_LXP_CODE_SIZE ((size_t)OVE_MEMORY_FLASH_KB * 1024u)
#endif
#if defined(OVE_MEMORY_RAM_START) && defined(OVE_MEMORY_RAM_KB)
#define OVE_LXP_TRUSTED_TCB_BASE ((uintptr_t)OVE_MEMORY_RAM_START)
#define OVE_LXP_TRUSTED_TCB_END (OVE_LXP_TRUSTED_TCB_BASE + (uintptr_t)OVE_MEMORY_RAM_KB * 1024u)
#endif

#endif /* LXP_OVE_MEMORY_LAYOUT_H */
