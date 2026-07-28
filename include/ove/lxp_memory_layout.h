/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LXP_MEMORY_LAYOUT_H
#define OVE_LXP_MEMORY_LAYOUT_H

#include <stdint.h>

#include "ove_config.h"

#if !defined(CONFIG_OVE_LINUX)
#error "ove/lxp_memory_layout.h is only valid for Linux-personality builds"
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

#endif /* OVE_LXP_MEMORY_LAYOUT_H */
