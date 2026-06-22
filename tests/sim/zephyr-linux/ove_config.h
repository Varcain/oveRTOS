/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * oveRTOS config for the isolated Zephyr Linux-personality bring-up: just the
 * engine-agnostic layers the SVC seam dispatches into (bounded arena + Linux
 * syscall core). The RTOS backing is Zephyr's own kernel, reached directly.
 */

#ifndef OVE_CONFIG_ZEPHYR_LINUX_H
#define OVE_CONFIG_ZEPHYR_LINUX_H

#define CONFIG_OVE_ARENA 1
#define CONFIG_OVE_LOADER 1
#define CONFIG_OVE_LINUX 1

#endif /* OVE_CONFIG_ZEPHYR_LINUX_H */
