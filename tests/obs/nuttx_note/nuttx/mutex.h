/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stand-in for <nuttx/mutex.h>: only the opaque handle types the NuttX
 * storage header declares are needed.  No locking happens in the test.
 */

#ifndef OVE_TEST_FAKE_NUTTX_MUTEX_H
#define OVE_TEST_FAKE_NUTTX_MUTEX_H

typedef struct {
	int _opaque;
} mutex_t;

typedef struct {
	int _opaque;
} rmutex_t;

#endif /* OVE_TEST_FAKE_NUTTX_MUTEX_H */
