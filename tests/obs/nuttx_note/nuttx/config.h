/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stand-in for <nuttx/config.h>: the NuttX storage header
 * (backends/nuttx/include/ove_storage_nuttx.h) is used unmodified so that
 * `struct ove_thread` is the real NuttX one; it only needs the NuttX
 * headers below, which carry no behaviour the observation code depends on.
 */

#ifndef OVE_TEST_FAKE_NUTTX_CONFIG_H
#define OVE_TEST_FAKE_NUTTX_CONFIG_H

#define CONFIG_OVE_RTOS_NUTTX 1

#endif /* OVE_TEST_FAKE_NUTTX_CONFIG_H */
