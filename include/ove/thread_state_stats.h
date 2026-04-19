/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @brief Per-thread state time tracking.
 *
 * Gated behind CONFIG_OVE_THREAD_STATE_STATS.  Each backend embeds
 * an ove_state_tracker in its thread struct and calls
 * ove_state_track_transition() on every state change.
 *
 * State indices match ove_thread_state_t:
 *   0=RUNNING, 1=READY, 2=BLOCKED, 3=SUSPENDED, 4=TERMINATED
 */

#ifndef OVE_THREAD_STATE_STATS_H
#define OVE_THREAD_STATE_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_THREAD_STATE_STATS

#define OVE_STATE_COUNT 5

/**
 * @brief Per-thread state-occupancy tracker.
 *
 * Accumulates time spent in each of `OVE_STATE_COUNT` states since init.
 * Call `ove_state_track_transition()` on every state change to update.
 */
struct ove_state_tracker {
	uint64_t cumul_us[OVE_STATE_COUNT]; /**< Cumulative microseconds spent in each state. */
	uint64_t last_ts_us;                /**< Timestamp of the last transition. */
	int      cur_state;                 /**< Current state index (0..OVE_STATE_COUNT-1). */
};

/** Platform-specific: return monotonic time in microseconds. */
uint64_t ove_state_stats_now_us(void);

static inline void ove_state_track_init(struct ove_state_tracker *st,
					int initial_state)
{
	for (int i = 0; i < OVE_STATE_COUNT; i++) st->cumul_us[i] = 0;
	st->last_ts_us = ove_state_stats_now_us();
	st->cur_state = initial_state;
}

static inline void ove_state_track_transition(struct ove_state_tracker *st,
					      int new_state)
{
	uint64_t now = ove_state_stats_now_us();
	int idx = st->cur_state;
	if (idx >= 0 && idx < OVE_STATE_COUNT)
		st->cumul_us[idx] += now - st->last_ts_us;
	st->last_ts_us = now;
	st->cur_state = new_state;
}

#else /* !CONFIG_OVE_THREAD_STATE_STATS */

struct ove_state_tracker { char _dummy; };
#define ove_state_track_init(st, s)          ((void)0)
#define ove_state_track_transition(st, s)    ((void)0)

#endif /* CONFIG_OVE_THREAD_STATE_STATS */

#ifdef __cplusplus
}
#endif

#endif /* OVE_THREAD_STATE_STATS_H */
