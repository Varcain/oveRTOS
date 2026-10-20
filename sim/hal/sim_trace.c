/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Trace simulation plugin.
 *
 * Drains the in-kernel trace ring (backends/common/ove_trace_ring.c) and
 * forwards records to the dashboard as ove_sim_event batches. Previously
 * owned a dedicated drain thread; that work is now driven by the
 * consolidated sim-debug pump via ove_sim_trace_tick().
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_TRACE_STREAM

#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_trace.h"
#include "ove/types.h"

#include "ove_trace_ring.h"

#include <stdio.h>
#include <string.h>

struct sim_trace_ctx {
	uint32_t plugin_id;
	uint32_t ms_since_desc;
};

static struct sim_trace_ctx trace_ctx;

/* ── STREAM emission ──────────────────────────────────────────────── */

/* Envelope is 1+1+2+4 = 8 bytes followed by @count * 16 byte records. */
#define STREAM_ENVELOPE_BYTES 8
#define STREAM_BUF_BYTES \
	(STREAM_ENVELOPE_BYTES + OVE_SIM_TRACE_MAX_BATCH * sizeof(struct ove_trace_record))

_Static_assert(sizeof(struct ove_sim_event) + STREAM_BUF_BYTES <= OVE_SIM_EVENT_MAX_BYTES,
	       "trace batch exceeds OVE_SIM_EVENT_MAX_BYTES");
_Static_assert(sizeof(struct ove_trace_record) == 16, "trace record wire size is 16 bytes");

/* Pump-scoped scratch. The trace plugin emits from the consolidated
 * sim-debug pump thread only, so these are single-threaded. Keeping them
 * in BSS avoids a buffer-sized stack spike on the embedded RTOSes' pump
 * stack. */
static uint8_t stream_ev_buf[sizeof(struct ove_sim_event) + STREAM_BUF_BYTES];

/* Records and ring drops of stream batches the transport refused (it never
 * laps the host, see guest_send_event()), reported in the next batch's
 * drop count.  Pump thread only. */
static uint32_t trace_refused;

/* Returns OVE_OK, or the transport's error when the batch was not sent. */
static int emit_stream_batch(struct sim_trace_ctx *t, const struct ove_trace_record *recs, size_t n,
			     uint32_t dropped)
{
	if (n == 0 && dropped == 0)
		return OVE_OK;

	struct ove_sim_event *ev = (struct ove_sim_event *)stream_ev_buf;
	uint8_t *p = ev->data;

	*p++ = OVE_SIM_TRACE_SUB_STREAM;
	*p++ = OVE_SIM_TRACE_VERSION;
	uint16_t count16 = (uint16_t)n;
	memcpy(p, &count16, 2);
	p += 2;
	memcpy(p, &dropped, 4);
	p += 4;

	size_t rec_bytes = n * sizeof(struct ove_trace_record);
	memcpy(p, recs, rec_bytes);
	p += rec_bytes;

	ev->plugin_id = t->plugin_id;
	ev->event_type = OVE_SIM_TRACE_EVT_STREAM;
	ev->timestamp_ms = 0;
	ev->data_len = (uint32_t)(p - ev->data);

	return ove_sim_plugin_emit_event(t->plugin_id, ev);
}

/* ── DESCRIPTORS emission ────────────────────────────────────────── */

/* Envelope (8 B) + per thread (4 tid + 4 generation + 1 len + up to 31 name). */
#define DESC_PER_MAX (4 + 4 + 1 + OVE_SIM_TRACE_MAX_NAME)
#define DESC_BUF_BYTES (STREAM_ENVELOPE_BYTES + OVE_SIM_TRACE_MAX_DESC * DESC_PER_MAX)

_Static_assert(sizeof(struct ove_sim_event) + DESC_BUF_BYTES <= OVE_SIM_EVENT_MAX_BYTES,
	       "trace descriptor batch exceeds OVE_SIM_EVENT_MAX_BYTES");

static uint8_t desc_ev_buf[sizeof(struct ove_sim_event) + DESC_BUF_BYTES];
static struct ove_trace_thread_desc desc_scratch[OVE_SIM_TRACE_MAX_DESC];
/* Last emitted (tid, generation) set; lets us push a fresh descriptor as
 * soon as a thread appears or its storage is reused, instead of waiting
 * for the periodic refresh. */
static uint32_t desc_last_tid[OVE_SIM_TRACE_MAX_DESC];
static uint32_t desc_last_gen[OVE_SIM_TRACE_MAX_DESC];
static size_t desc_last_count;

/* Whether the (tid, generation) set differs from the last one sent.  The
 * order does not matter: FreeRTOS lists tasks in state-list order, which
 * changes all the time without any thread starting or ending. */
static bool descriptors_changed(size_t count)
{
	if (count != desc_last_count)
		return true;
	for (size_t i = 0; i < count; i++) {
		bool found = false;
		for (size_t j = 0; j < desc_last_count && !found; j++)
			found = desc_scratch[i].tid == desc_last_tid[j] &&
				desc_scratch[i].generation == desc_last_gen[j];
		if (!found)
			return true;
	}
	return false;
}

static void descriptors_remember(size_t count)
{
	for (size_t i = 0; i < count; i++) {
		desc_last_tid[i] = desc_scratch[i].tid;
		desc_last_gen[i] = desc_scratch[i].generation;
	}
	desc_last_count = count;
}

/* Returns OVE_OK, or the transport's error when the batch was not sent. */
static int emit_descriptors(struct sim_trace_ctx *t, size_t count)
{
	if (count == 0)
		return OVE_OK;

	struct ove_sim_event *ev = (struct ove_sim_event *)desc_ev_buf;
	uint8_t *p = ev->data;
	uint8_t *end = ev->data + DESC_BUF_BYTES;

	*p++ = OVE_SIM_TRACE_SUB_DESCRIPTORS;
	*p++ = OVE_SIM_TRACE_VERSION;
	uint16_t count16 = (uint16_t)count;
	memcpy(p, &count16, 2);
	p += 2;
	uint32_t zero = 0;
	memcpy(p, &zero, 4);
	p += 4;

	for (size_t i = 0; i < count; i++) {
		const char *name = desc_scratch[i].name ? desc_scratch[i].name : "?";
		size_t nlen = strlen(name);
		if (nlen > OVE_SIM_TRACE_MAX_NAME)
			nlen = OVE_SIM_TRACE_MAX_NAME;
		if (p + 9 + nlen > end)
			break;
		memcpy(p, &desc_scratch[i].tid, 4);
		p += 4;
		memcpy(p, &desc_scratch[i].generation, 4);
		p += 4;
		*p++ = (uint8_t)nlen;
		memcpy(p, name, nlen);
		p += nlen;
	}

	ev->plugin_id = t->plugin_id;
	ev->event_type = OVE_SIM_TRACE_EVT_DESCRIPTORS;
	ev->timestamp_ms = 0;
	ev->data_len = (uint32_t)(p - ev->data);

	int ret = ove_sim_plugin_emit_event(t->plugin_id, ev);
	/* Remember the set only once the host has it: a refused batch is
	 * sent again on the next tick. */
	if (ret == OVE_OK)
		descriptors_remember(count);
	return ret;
}

/* ── Pump-driven tick ─────────────────────────────────────────────── */

static struct ove_trace_record drain_batch[OVE_SIM_TRACE_MAX_BATCH];

void ove_sim_trace_tick(uint32_t elapsed_ms)
{
	size_t n;
	do {
		n = ove_trace_ring_drain(drain_batch, OVE_SIM_TRACE_MAX_BATCH);
		uint32_t dropped = ove_trace_ring_dropped_get() + trace_refused;
		trace_refused = 0;
		/* A batch the transport refused is lost: carry its records and
		 * the drops it reported into the next batch's count. */
		if (emit_stream_batch(&trace_ctx, drain_batch, n, dropped) != OVE_OK)
			trace_refused = dropped + (uint32_t)n;
	} while (n == OVE_SIM_TRACE_MAX_BATCH);

	/* Enumerate every tick so a thread start / storage reuse is reported
	 * immediately (a fresh generation starts a new dashboard lane) rather
	 * than up to OVE_SIM_TRACE_DESC_MS later. */
	size_t count = ove_backend_trace_list_threads(desc_scratch, OVE_SIM_TRACE_MAX_DESC);
	trace_ctx.ms_since_desc += elapsed_ms;
	bool due = trace_ctx.ms_since_desc >= OVE_SIM_TRACE_DESC_MS;
	if (count > 0 && (due || descriptors_changed(count)) &&
	    emit_descriptors(&trace_ctx, count) == OVE_OK)
		trace_ctx.ms_since_desc = 0;
}

/* ── Plugin ops ───────────────────────────────────────────────────── */

static int trace_init(void *ctx, const void *config, size_t config_len)
{
	(void)config;
	(void)config_len;
	(void)ctx;
	return OVE_OK;
}

static const struct ove_sim_plugin_ops trace_ops = {
	.name = "trace",
	.type = OVE_SIM_PLUGIN_SENSOR,
	.init = trace_init,
};

int ove_sim_trace_register(void)
{
	int id = ove_sim_plugin_register(&trace_ops, &trace_ctx, NULL, 0);
	if (id < 0)
		return id;

	trace_ctx.plugin_id = (uint32_t)id;
	trace_ctx.ms_since_desc = 0;
	return id;
}

#else /* !CONFIG_OVE_TRACE_STREAM */

#include "ove/sim/ove_sim_trace.h"

int ove_sim_trace_register(void)
{
	return 0;
}
void ove_sim_trace_tick(uint32_t elapsed_ms)
{
	(void)elapsed_ms;
}

#endif /* CONFIG_OVE_TRACE_STREAM */
