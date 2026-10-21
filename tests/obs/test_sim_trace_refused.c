/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* Host test for trace backlog, overflow accounting, and descriptor emission. */

#include <stdio.h>
#include <string.h>

#include "ove/sim/ove_sim_trace.h"
#include "ove/types.h"
#include "ove_trace_ring.h"

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

static bool refuse;
static unsigned stream_refusals, descriptor_attempts, accepted_batches;
static uint32_t reported_records, reported_dropped;

int ove_sim_plugin_register(const struct ove_sim_plugin_ops *ops, void *ctx, const void *config,
			    size_t config_len)
{
	return 1;
}

size_t ove_backend_trace_list_threads(struct ove_trace_thread_desc *out, size_t max)
{
	*out = (struct ove_trace_thread_desc){.tid = 1, .name = "worker"};
	return 1;
}

int ove_sim_plugin_emit_event(uint32_t plugin_id, const struct ove_sim_event *event)
{
	if (event->event_type == OVE_SIM_TRACE_EVT_DESCRIPTORS) {
		descriptor_attempts++;
		return refuse ? OVE_ERR_QUEUE_FULL : OVE_OK;
	}
	CHECK(event->event_type == OVE_SIM_TRACE_EVT_STREAM);
	if (refuse) {
		stream_refusals++;
		return OVE_ERR_QUEUE_FULL;
	}

	uint16_t count;
	uint32_t dropped;
	memcpy(&count, event->data + 2, sizeof(count));
	memcpy(&dropped, event->data + 4, sizeof(dropped));
	for (uint16_t i = 0; i < count; i++) {
		uint64_t ts_us;
		memcpy(&ts_us, event->data + 8 + i * sizeof(struct ove_trace_record),
		       sizeof(ts_us));
		CHECK(ts_us == OVE_SIM_TRACE_MAX_BATCH + reported_records + i);
	}
	accepted_batches++;
	reported_records += count;
	reported_dropped += dropped;
	return OVE_OK;
}

int main(void)
{
	CHECK(ove_sim_trace_register() >= 0);
	for (unsigned i = 0; i < 3 * OVE_SIM_TRACE_MAX_BATCH; i++) {
		const struct ove_trace_record record = {.ts_us = i};
		CHECK(ove_trace_ring_push(&record));
	}
	CHECK(!ove_trace_ring_push(&(struct ove_trace_record){0}));

	refuse = true;
	ove_sim_trace_tick(OVE_SIM_TRACE_DRAIN_MS);
	CHECK(stream_refusals == 1);
	CHECK(descriptor_attempts == 1);
	CHECK(accepted_batches == 0);

	refuse = false;
	ove_sim_trace_tick(OVE_SIM_TRACE_DRAIN_MS);
	CHECK(accepted_batches == 2);
	CHECK(reported_records == 2 * OVE_SIM_TRACE_MAX_BATCH);
	CHECK(reported_dropped == OVE_SIM_TRACE_MAX_BATCH + 1);

	ove_sim_trace_tick(OVE_SIM_TRACE_DRAIN_MS);
	CHECK(accepted_batches == 2);

	if (failures)
		return 1;
	puts("test_sim_trace_refused: OK");
	return 0;
}
