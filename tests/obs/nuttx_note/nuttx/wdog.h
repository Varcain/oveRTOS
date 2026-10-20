/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stand-in for <nuttx/wdog.h>: opaque watchdog handle for the NuttX storage
 * header.  Unused by the observation code under test.
 */

#ifndef OVE_TEST_FAKE_NUTTX_WDOG_H
#define OVE_TEST_FAKE_NUTTX_WDOG_H

struct wdog_s {
	int _opaque;
};

#endif /* OVE_TEST_FAKE_NUTTX_WDOG_H */
