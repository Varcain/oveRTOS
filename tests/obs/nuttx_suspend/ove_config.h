/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Build configuration of the NuttX suspend/resume regression test
 * (tests/obs/nuttx_suspend/).  It selects the NuttX backend's storage
 * header, because the handshake under test operates on `struct
 * ove_thread` (the NuttX one).
 *
 * The scheduler-note observation (CONFIG_OVE_THREAD_STATE_STATS) is
 * deliberately off: the handshake does not touch it, and the fake NuttX
 * headers of this test do not model the note driver.
 */

#ifndef OVE_CONFIG_H
#define OVE_CONFIG_H

#define CONFIG_OVE_RTOS_NUTTX 1

#endif /* OVE_CONFIG_H */
