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
 * sent again on the next tick.  The rate guard lowers the sampling rate
 * only when an accepted, saturated drain still sees the ring overflow -
 * a tick the transport refused is not a rate problem - and a burst of
 * rate commands costs one caps event.
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

static uint32_t backend_rate;

void ove_backend_profiler_set_rate(uint32_t hz)
{
	backend_rate = hz;
}

uint32_t ove_backend_profiler_get_max_hz(void)
{
	return 1000;
}

uint32_t ove_backend_profiler_get_flags(void)
{
	return 0;
}

/* The registry keeps the plugin, so the test can init it and send it
 * commands the way the pump does. */
static const struct ove_sim_plugin_ops *plugin_ops;
static void *plugin_ctx;

int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops, void *ctx, const void *config,
			    size_t config_len)
{
	(void)config;
	(void)config_len;
	plugin_ops = ops;
	plugin_ctx = ctx;
	return 1;
}

/* The transport: refuses while refuse > 0; records what it accepts.  While
 * refill > 0 every samples event (accepted or refused) is followed by
 * refill_n new samples, a producer that keeps the drain saturated. */
static int refuse;
static unsigned refill, refill_n;
static unsigned accepted_samples, accepted_caps;
static uint32_t reported_dropped, reported_samples;
static uint32_t caps_max_hz, caps_current_hz;

static void push(unsigned n);

int ove_sim_plugin_emit_event(uint32_t plugin_id, const struct ove_sim_event *event)
{
	(void)plugin_id;
	if (event->event_type == OVE_SIM_PROFILER_EVT_SAMPLES && refill) {
		refill--;
		push(refill_n);
	}
	if (refuse) {
		refuse--;
		return OVE_ERR_QUEUE_FULL;
	}
	if (event->event_type == OVE_SIM_PROFILER_EVT_CAPS) {
		accepted_caps++;
		memcpy(&caps_max_hz, event->data + 0, 4);
		memcpy(&caps_current_hz, event->data + 4, 4);
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

/* Empty the sample ring and settle the drop and caps carries. */
static void settle(void)
{
	refuse = 0;
	refill = 0;
	for (int i = 0; i < 4; i++)
		ove_sim_profiler_tick();
	reset_counts();
}

static void set_rate_cmd(uint32_t hz)
{
	uint8_t buf[sizeof(struct ove_sim_cmd) + 4];
	struct ove_sim_cmd *cmd = (struct ove_sim_cmd *)buf;
	memset(buf, 0, sizeof(buf));
	cmd->cmd_type = OVE_SIM_PROFILER_CMD_SET_RATE;
	cmd->data_len = 4;
	memcpy(cmd->data, &hz, 4);
	CHECK(plugin_ops->handle_cmd(plugin_ctx, cmd) == OVE_OK);
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

/* The announcement is sent by the next tick; a refused one is sent again
 * by the tick after that. */
static void test_refused_caps_resent(void)
{
	reset_counts();
	ove_sim_profiler_announce_caps();
	CHECK(accepted_caps == 0);
	refuse = 1;
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 0);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
}

/* A whole saturated tick refused while the ring overflows: the samples
 * never reached the host, so the drain says nothing about the sampling
 * rate, and the rate stays where it was. */
static void test_refused_tick_keeps_the_rate(void)
{
	settle();
	set_rate_cmd(1000);
	settle();
	CHECK(backend_rate == 1000);

	push(CONFIG_OVE_PROFILER_RING_SIZE + 50);
	refill = 1000;
	refill_n = CONFIG_OVE_PROFILER_RING_SIZE;
	refuse = 1000;
	ove_sim_profiler_tick();
	CHECK(backend_rate == 1000);
	settle();
	CHECK(backend_rate == 1000);
}

/* An accepted, saturated drain that still sees the ring overflow lowers
 * the rate, and the next caps event says so - max_hz stays the backend's
 * ceiling, current_hz is the selected rate. */
static void test_overflow_lowers_the_rate(void)
{
	settle();
	set_rate_cmd(1000);
	settle();

	push(CONFIG_OVE_PROFILER_RING_SIZE + 50);
	refill = 1000;
	refill_n = CONFIG_OVE_PROFILER_RING_SIZE;
	ove_sim_profiler_tick();
	CHECK(backend_rate < 1000 && backend_rate > 0);
	uint32_t lowered = backend_rate;
	refill = 0;
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
	CHECK(caps_max_hz == 1000);
	CHECK(caps_current_hz == lowered);
}

/* Any number of rate commands in one pump iteration costs one caps event,
 * carrying the last rate. */
static void test_rate_commands_cost_one_caps_event(void)
{
	settle();
	for (uint32_t hz = 1; hz <= 100; hz++)
		set_rate_cmd(hz * 10u);
	CHECK(accepted_caps == 0);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
	CHECK(caps_current_hz == 1000);
	ove_sim_profiler_tick();
	CHECK(accepted_caps == 1);
}

int main(void)
{
	CHECK(ove_sim_profiler_register() >= 0);
	CHECK(plugin_ops && plugin_ops->init && plugin_ops->handle_cmd);
	CHECK(plugin_ops->init(plugin_ctx, NULL, 0) == OVE_OK);
	CHECK(backend_rate == 1000);
	settle();
	test_refused_batch_reported_once();
	test_overflow_and_refusal_add_up();
	test_refused_caps_resent();
	test_refused_tick_keeps_the_rate();
	test_overflow_lowers_the_rate();
	test_rate_commands_cost_one_caps_event();

	if (failures) {
		fprintf(stderr, "test_sim_profiler_refused: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_sim_profiler_refused: OK\n");
	return 0;
}
