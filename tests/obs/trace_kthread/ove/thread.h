/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Stand-in for ove/thread.h: the kernel-thread tracker only needs the
 * thread states and the trace clock. */

#ifndef OVE_THREAD_H
#define OVE_THREAD_H

#include <stdint.h>

typedef enum {
	OVE_THREAD_STATE_RUNNING = 0,
	OVE_THREAD_STATE_READY,
	OVE_THREAD_STATE_BLOCKED,
	OVE_THREAD_STATE_SUSPENDED,
	OVE_THREAD_STATE_TERMINATED,
	OVE_THREAD_STATE_UNKNOWN,
} ove_thread_state_t;

uint64_t ove_state_stats_now_us(void);

#endif /* OVE_THREAD_H */
