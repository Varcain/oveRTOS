/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_CONFIG_NUTTX_QEMU_LINUX_H
#define OVE_CONFIG_NUTTX_QEMU_LINUX_H

/*
 * Minimal config for the isolated Linux-personality on-target test: only the
 * modules the bFLT/SVC-trap test links — the arena, the module loader, and the
 * Linux personality. No full backend/suite, so the 291-test combined-suite
 * corruption cannot occur here.
 */
#define CONFIG_OVE_RTOS_NUTTX 1
#define CONFIG_OVE_ARENA 1
#define CONFIG_OVE_LOADER 1
#define CONFIG_OVE_LINUX 1

#endif /* OVE_CONFIG_NUTTX_QEMU_LINUX_H */
