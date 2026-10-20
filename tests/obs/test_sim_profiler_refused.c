/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the profiler drain (sim/hal/sim_profiler.c) against a
 * transport that refuses events: the samples and drops of a refused batch
 * are reported once, in the next batch's drop count, without going back
 * into the sample ring's own overflow counter; a refused caps event is
 * sent again on the next tick.
 */

#include <stdio.h>
#include <string.h>

#include "ove/profiler.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_profiler.h"
#include "ove/types.h"
#include "ove_profiler_ring.h"

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

/* ── stand-ins for the backend and the plugin registry ─────────────── */

int ove_backend_profiler_start(void)
{
	return OVE_OK;
}

void ove_backend_profiler_stop(void)
{
}

void ove_backend_profiler_set_rate(uint32_t hz)
{
	(void)hz;
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	return 1000;
}

uint32_t ove_backend_profiler_get_flags(void)
{
	return 0;
}

int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops, void *ctx, const void *config,
			    size_t config_len)
{
	(void)ops;
	(void)ctx;
	(void)config;
	(void)config_len;
	return 1;
}

/* The transport: refuses while refuse > 0; records what it accepts. */
static int refuse;
static unsigned accepted_samples, accepted_caps;
static uint32_t reported_dropped, reported_samples;

int ove_sim_plugin_emit_event(uint32_t plugin_id, const struct ove_sim_event *event)
{
	(void)plugin_id;
	if (refuse) {
		refuse--;
		return OVE_ERR_QUEUE_FULL;
	}
	if (event->event_type == OVE_SIM_PROFILER_EVT_CAPS) {
		accepted_caps++;
	} else if (event->event_type == OVE_SIM_PROFILER_EVT_SAMPLES) {
		uint16_t count;
		uint32_t dropped;
		memcpy(&count, event->data + 2, 2);
		memcpy(&dropped, event->data + 4, 4);
		accepted_samples++;
		reported_samples += count;
		reported_dropped += dropped;
	}
	return OVE_OK;
}

/* ── helpers ────────────────────────────────────────────────────────── */

static void push(unsigned n)
{
	struct ove_profiler_sample s;
	memset(&s, 0, sizeof(s));
	s.depth = 1;
	s.pcs[0] = 0x1000;
	for (unsigned i = 0; i < n; i++) {
		s.ts_us = i;
		(void)ove_profiler_ring_push(&s);
	}
}

static void reset_counts(void)
{
	accepted_samples = accepted_caps = 0;
	reported_dropped = reported_samples = 0;
}

/* ── scenarios ──────────────────────────────────────────────────────── */

/* A refused batch is lost: its samples show up once as drops in the next
 * batch, and the ring's own overflow counter stays out of it. */
static void test_refused_batch_reported_once(void)
{
	reset_counts();
	push(10);
	refuse = 1;
	ove_sim_profiler_tick();
	CHECK(accepted_samples == 0);
	CHECK(ove_profiler_ring_dropped_get() == 0); /* not the ring's overflow */

	ove_sim_profiler_tick();
	CHECK(accepted_samples == 1);
	CHECK(reported_samples == 0);
	CHECK(reported_dropped == 10);

	ove_sim_profiler_tick(); /* nothing left to report */
	CHECK(reported_dropped == 10);
}

/* Ring overflow and refusal together: every sample is either delivered or
 * counted as dropped, exactly once. */
static void test_overflow_and_refusal_add_up(void)
{
	const unsigned pushed = CONFIG_OVE_PROFILER_RING_SIZE + 37;

	reset_counts();
	push(pushed);
	refuse = 1000; /* the whole tick is refused */
	ove_sim_profiler_tick();
	refuse = 0;
	CHECK(accepted_samples == 0);

	push(5);
	ove_sim_profiler_tick();
	ove_sim_profiler_tick();
	CHECK(reported_samples + reported_dropped == pushed + 5);
	CHECK(reported_samples == 5);
}

/* A refused caps announcement is sent again by the next tick. */
static void test_refused_caps_resent(void)
{
	reset_counts();
	refuse = 1;
	ove_sim_profiler_announce_caps();
	CHECK(accepted_caps == 0);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
}

int main(void)
{
	CHECK(ove_sim_profiler_register() >= 0);
	test_refused_batch_reported_once();
	test_overflow_and_refusal_add_up();
	test_refused_caps_resent();

	if (failures) {
		fprintf(stderr, "test_sim_profiler_refused: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_sim_profiler_refused: OK\n");
	return 0;
}
