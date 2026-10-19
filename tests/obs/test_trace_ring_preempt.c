/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Deterministic preemption test for ove_trace_ring_push().
 *
 * The ring is built by trace_ring_preempt.c with a hook before every
 * atomic load.  For each fill level, nested action and load of the
 * first attempt the test arms the hook inside one outer push and runs
 * the nested context there (a producer, the drainer, or both),
 * optionally once more at the next load, which lands in the retry path
 * the first preemption may have caused.  It then checks:
 *   - the outer push fails only if the ring was full at some point of it;
 *   - every accepted record is drained exactly once, in per-producer order;
 *   - never more than the ring size is outstanding;
 *   - the dropped counter equals the number of rejected pushes.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ove_config.h"
#include "ove_trace_ring.h"

#define RING CONFIG_OVE_TRACE_RING_SIZE
#define TID_FILL 1u
#define TID_NESTED 2u
#define TID_OUTER 9u

static int arm = -1;
static int rearm;
static int fired;
static int nested_action;
static int nested_ok;
static int nested_fail;
static size_t nested_drained;
static uint64_t seq_nested;
/* Model fill level and its peak while the outer push runs. */
static int occ;
static int peak;
static uint64_t last_ts[16];
static int bad_content;

static struct ove_trace_record make_rec(uint32_t tid, uint64_t ts)
{
	struct ove_trace_record r;
	memset(&r, 0, sizeof(r));
	r.tid = tid;
	r.ts_us = ts;
	return r;
}

static size_t drain_checked(void)
{
	struct ove_trace_record out[64];
	size_t n, total = 0;

	while ((n = ove_trace_ring_drain(out, 64)) > 0) {
		for (size_t i = 0; i < n; i++) {
			if (out[i].tid > 15 || out[i].ts_us <= last_ts[out[i].tid])
				bad_content++;
			else
				last_ts[out[i].tid] = out[i].ts_us;
		}
		total += n;
	}
	return total;
}

static void nested_push(int count)
{
	for (int i = 0; i < count; i++) {
		struct ove_trace_record r = make_rec(TID_NESTED, ++seq_nested);
		if (ove_trace_ring_push(&r)) {
			nested_ok++;
			if (++occ > peak)
				peak = occ;
		} else {
			nested_fail++;
		}
	}
}

static void nested_drain(void)
{
	size_t n = drain_checked();
	nested_drained += n;
	occ -= (int)n;
}

void trace_ring_preempt_point(void)
{
	if (arm <= 0 || --arm)
		return;
	arm = -1;
	fired = 1;
	switch (nested_action) {
	case 0: /* producer, then the drainer */
		nested_push(1);
		nested_drain();
		break;
	case 1: /* drainer only */
		nested_drain();
		break;
	case 2: /* producer only */
		nested_push(1);
		break;
	case 3: /* burst that fills the ring */
		nested_push(RING);
		break;
	case 4: /* drainer, then two producers */
		nested_drain();
		nested_push(2);
		break;
	case 5: /* three producers, then the drainer */
		nested_push(3);
		nested_drain();
		break;
	}
	/* Re-arm only now, so the second preemption hits the outer push and
	 * not the nested context that just ran. */
	if (rearm) {
		rearm = 0;
		arm = 1;
	}
}

#define N_ACTIONS 6

int main(void)
{
	static const int fills[] = {0, 1, 2, RING - 2, RING - 1, RING};
	int failures = 0, cases = 0;
	uint64_t seq_fill = 0;

	for (unsigned fi = 0; fi < sizeof(fills) / sizeof(fills[0]); fi++) {
		for (int a = 0; a < N_ACTIONS; a++) {
			for (int k = 1; k <= 4; k++) {
				int twice = k > 2;
				int load = twice ? k - 2 : k;

				arm = -1;
				rearm = 0;
				drain_checked();
				(void)ove_trace_ring_dropped_get();
				seq_nested = 0;
				bad_content = 0;
				nested_ok = nested_fail = 0;
				nested_drained = 0;
				fired = 0;

				int fill_ok = 0;
				for (int i = 0; i < fills[fi]; i++) {
					struct ove_trace_record r = make_rec(TID_FILL, ++seq_fill);
					fill_ok += ove_trace_ring_push(&r);
				}
				memset(last_ts, 0, sizeof(last_ts));
				occ = peak = fill_ok;

				/* Outer push, preempted before atomic load `load`
				 * and, if `twice`, again before the next one. */
				nested_action = a;
				rearm = twice;
				arm = load;
				struct ove_trace_record outer = make_rec(TID_OUTER, 1);
				int ok = ove_trace_ring_push(&outer);
				if (!fired) {
					arm = -1; /* the push did fewer loads */
					drain_checked();
					continue;
				}
				arm = -1;
				cases++;

				uint32_t dropped = ove_trace_ring_dropped_get();
				size_t rest = drain_checked();
				int accepted = fill_ok + nested_ok + ok;
				const char *why = NULL;

				if (!ok && peak < RING)
					why = "dropped on a ring that was never full";
				else if ((size_t)accepted != nested_drained + rest)
					why = "accepted records lost or duplicated";
				else if (bad_content)
					why = "records reordered or corrupted";
				else if (rest > RING)
					why = "more than the ring size outstanding";
				else if (dropped != (uint32_t)(nested_fail + !ok))
					why = "dropped counter disagrees with rejected pushes";

				if (why) {
					fprintf(stderr,
						"fill=%d action=%d load=%d%s: %s (push=%d peak=%d "
						"dropped=%u)\n",
						fills[fi], a, load, twice ? "+next" : "", why, ok,
						peak, (unsigned)dropped);
					failures++;
				}
			}
		}
	}

	if (cases == 0) {
		fprintf(stderr, "no preemption point fired\n");
		failures++;
	}
	if (failures) {
		fprintf(stderr, "test_trace_ring_preempt: %d of %d cases FAILED\n", failures,
			cases);
		return 1;
	}
	printf("test_trace_ring_preempt: OK (%d cases)\n", cases);
	return 0;
}
