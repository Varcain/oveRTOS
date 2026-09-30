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

/* board_desc.h defines the board descriptor object, so include it only where its
 * memory and display facts are used. */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#include "board_desc.h"
#endif

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

/*
 * PMSAv7 cannot express a 12 MiB region. FreeRTOS/AN500 has four task MPU
 * descriptors available, so it represents the configured range as adjacent
 * 8 MiB and 4 MiB windows. Every other supported layout uses one window.
 */
#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) && defined(CONFIG_OVE_RTOS_FREERTOS)
#define OVE_LXP_ROOTFS_MPU_WINDOW_COUNT 2
#define OVE_LXP_ROOTFS_MPU0_BASE OVE_LXP_ROOTFS_BASE
#define OVE_LXP_ROOTFS_MPU0_SIZE ((uintptr_t)0x00800000u)
#define OVE_LXP_ROOTFS_MPU1_BASE (OVE_LXP_ROOTFS_BASE + OVE_LXP_ROOTFS_MPU0_SIZE)
#define OVE_LXP_ROOTFS_MPU1_SIZE (OVE_LXP_ROOTFS_SIZE - OVE_LXP_ROOTFS_MPU0_SIZE)
#else
#define OVE_LXP_ROOTFS_MPU_WINDOW_COUNT 1
#define OVE_LXP_ROOTFS_MPU0_BASE OVE_LXP_ROOTFS_BASE
#define OVE_LXP_ROOTFS_MPU0_SIZE OVE_LXP_ROOTFS_SIZE
#endif

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

/* Where the NuttX port's guests may execute from (its MPU code region) and the
 * window every native task control block must lie in. */
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
#define OVE_LXP_CODE_BASE ((uintptr_t)0x08000000u) /* internal flash */
#define OVE_LXP_CODE_SIZE ((size_t)OVE_MEMORY_FLASH_KB * 1024u)
#define OVE_LXP_TRUSTED_TCB_BASE ((uintptr_t)0x20000000u)
#define OVE_LXP_TRUSTED_TCB_END ((uintptr_t)0x20080000u)
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
#define OVE_LXP_CODE_BASE ((uintptr_t)0x00000000u)
#define OVE_LXP_CODE_SIZE ((size_t)2u * 1024u * 1024u)
#define OVE_LXP_TRUSTED_TCB_BASE ((uintptr_t)0x20000000u)
#define OVE_LXP_TRUSTED_TCB_END ((uintptr_t)0x20400000u)
#endif

#endif /* LXP_OVE_MEMORY_LAYOUT_H */
