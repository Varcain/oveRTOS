/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for ove_state_track_read(), the per-state snapshot every RTOS
 * backend's ove_thread_list() reports: the open interval goes to the
 * current state only, a clock that steps back adds nothing, and over any
 * run of transitions the snapshot sums to the elapsed time and never goes
 * down between reads.
 */

#include <stdint.h>
#include <stdio.h>

#define CONFIG_OVE_THREAD_STATE_STATS 1
#include "ove/thread_state_stats.h"

static uint64_t fake_now;
static int failures;

uint64_t ove_state_stats_now_us(void)
{
	return fake_now;
}

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

static uint64_t sum(const uint64_t t[OVE_STATE_COUNT])
{
	uint64_t s = 0;
	for (int i = 0; i < OVE_STATE_COUNT; i++)
		s += t[i];
	return s;
}

/* The open interval since the last transition lands in the current state. */
static void check_fold(void)
{
	for (int state = 0; state < OVE_STATE_COUNT; state++) {
		struct ove_state_tracker st;
		uint64_t t[OVE_STATE_COUNT];
		fake_now = 1000;
		ove_state_track_init(&st, 1);
		ove_state_track_transition_at(&st, state, 1300); /* 300 us READY */
		ove_state_track_read(&st, 1750, t);
		for (int i = 0; i < OVE_STATE_COUNT; i++) {
			uint64_t want = (i == 1 ? 300u : 0u) + (i == state ? 450u : 0u);
			CHECK(t[i] == want);
		}
	}
}

/* A clock read behind the last transition adds no open interval, and a
 * corrupt state index is not used as one. */
static void check_guards(void)
{
	struct ove_state_tracker st;
	uint64_t t[OVE_STATE_COUNT];
	fake_now = 5000;
	ove_state_track_init(&st, 0);
	ove_state_track_transition_at(&st, 2, 6000);
	ove_state_track_read(&st, 5999, t);
	CHECK(t[0] == 1000 && t[2] == 0 && sum(t) == 1000);

	/* Read between two sentinels: an index just outside the array would
	 * land on one of them. */
	uint64_t buf[OVE_STATE_COUNT + 2];
	const int bad[] = {OVE_STATE_COUNT, -1};
	for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		buf[0] = buf[OVE_STATE_COUNT + 1] = 0x5a5a5a5a5a5a5a5aull;
		st.cur_state = bad[k];
		ove_state_track_read(&st, 9000, buf + 1);
		CHECK(sum(buf + 1) == 1000);
		CHECK(buf[0] == 0x5a5a5a5a5a5a5a5aull);
		CHECK(buf[OVE_STATE_COUNT + 1] == 0x5a5a5a5a5a5a5a5aull);
	}
}

/* Interleave transitions and reads: every read sums to the time since init
 * and no state's total drops below the previous read's. */
static void check_run(void)
{
	struct ove_state_tracker st;
	uint64_t prev[OVE_STATE_COUNT] = {0};
	uint64_t t[OVE_STATE_COUNT];
	uint32_t rng = 12345;
	const uint64_t t0 = 0xfffff000u; /* cross the 32-bit boundary */
	uint64_t now = t0;
	unsigned bad_sum = 0, bad_mono = 0;

	fake_now = t0;
	ove_state_track_init(&st, 1);
	for (int step = 0; step < 100000; step++) {
		rng = rng * 1103515245u + 12345u;
		now += (rng >> 8) % 997;
		if (rng & 1) {
			ove_state_track_transition_at(&st, (int)((rng >> 4) % 4), now);
		} else {
			ove_state_track_read(&st, now, t);
			if (sum(t) != now - t0)
				bad_sum++;
			for (int i = 0; i < OVE_STATE_COUNT; i++) {
				if (t[i] < prev[i])
					bad_mono++;
				prev[i] = t[i];
			}
		}
	}
	CHECK(bad_sum == 0);
	CHECK(bad_mono == 0);
}

int main(void)
{
	check_fold();
	check_guards();
	check_run();
	if (failures) {
		fprintf(stderr, "test_state_track_read: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_state_track_read: OK\n");
	return 0;
}
