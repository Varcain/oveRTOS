/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef POSIX_SLEEP_H
#define POSIX_SLEEP_H

#include <errno.h>
#include <stdint.h>
#include <time.h>

/*
 * Uninterruptible short sleeps for the POSIX backend.
 *
 * usleep/nanosleep return EINTR when a signal interrupts them, and
 * SA_RESTART does not auto-restart timed sleeps. With the sampling
 * profiler delivering SIGRTMIN at CONFIG_OVE_PROFILER_HZ (default 250),
 * a naive usleep() call returns early on every sample — app timers,
 * counters and scheduled work then run fast. We use an absolute
 * CLOCK_MONOTONIC deadline so each resume continues toward the
 * original target time.
 */

static inline void posix_sleep_ns(uint64_t ns)
{
	struct timespec target;
	clock_gettime(CLOCK_MONOTONIC, &target);
	target.tv_sec += (time_t)(ns / 1000000000ULL);
	target.tv_nsec += (long)(ns % 1000000000ULL);
	if (target.tv_nsec >= 1000000000L) {
		target.tv_sec++;
		target.tv_nsec -= 1000000000L;
	}
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, NULL) == EINTR) {
	}
}

static inline void posix_sleep_ms(uint32_t ms)
{
	posix_sleep_ns((uint64_t)ms * 1000000ULL);
}

#endif /* POSIX_SLEEP_H */
