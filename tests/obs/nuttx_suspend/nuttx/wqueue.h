/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stand-in for <nuttx/wqueue.h>: opaque work item for the NuttX storage
 * header.  Unused by the suspend/resume handshake under test.
 */

#ifndef OVE_TEST_FAKE_NUTTX_WQUEUE_H
#define OVE_TEST_FAKE_NUTTX_WQUEUE_H

struct work_s {
	int _opaque;
};

#endif /* OVE_TEST_FAKE_NUTTX_WQUEUE_H */
