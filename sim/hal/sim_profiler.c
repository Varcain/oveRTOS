/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Profiler simulation plugin.
 *
 * Drains the sample ring populated by the RTOS sampler backend and
 * emits batches to the dashboard. Draining is driven by the
 * consolidated sim-debug pump via ove_sim_profiler_tick(). Sampling
 * itself is armed once at init via ove_backend_profiler_start().
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_PROFILER

#include "ove/profiler.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_profiler.h"
#include "ove/types.h"

#include "ove_profiler_ring.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct sim_profiler_ctx {
	uint32_t plugin_id;
	uint32_t current_hz;
};

static struct sim_profiler_ctx profiler_ctx;

static void emit_caps(struct sim_profiler_ctx *t)
{
	uint8_t payload[12];
	uint32_t max_hz = ove_backend_profiler_get_max_hz();
	uint32_t flags = ove_backend_profiler_get_flags();
	memcpy(payload + 0, &max_hz, 4);
	memcpy(payload + 4, &t->current_hz, 4);
	memcpy(payload + 8, &flags, 4);

	uint8_t ev_buf[sizeof(struct ove_sim_event) + sizeof(payload)];
	struct ove_sim_event *ev = (struct ove_sim_event *)ev_buf;
	ev->plugin_id = t->plugin_id;
	ev->event_type = OVE_SIM_PROFILER_EVT_CAPS;
	ev->timestamp_ms = 0;
	ev->data_len = sizeof(payload);
	memcpy(ev->data, payload, sizeof(payload));

	ove_sim_plugin_emit_event(t->plugin_id, ev);
}

/*
 * Envelope (1+1+2+4 = 8 B) + per sample:
 *   8 (ts) + 4 (tid) + 1 (depth) + 1 (state) + 2 (pad) + depth * 8 (pcs)
 *
 * The state byte is ove_thread_state_t captured at sample time so the
 * dashboard can filter on-CPU vs wall-clock views.
 *
 * We serialise each sample compactly (only @depth PCs, not the full
 * fixed slot) to avoid wasting bridge bandwidth on zero-padded tails.
 *
 * A batch holds as many full-depth samples as fit in one event of at most
 * OVE_SIM_EVENT_MAX_BYTES (the host side skips larger ones), so the batch
 * size follows CONFIG_OVE_PROFILER_MAX_DEPTH: 28 at depth 16, 7 at 64.
 */
#define PROFILE_ENVELOPE_BYTES 8u
#define SAMPLE_HDR_BYTES 16u
#define PC_WIRE_BYTES 8u
#define MAX_SAMPLE_BYTES (SAMPLE_HDR_BYTES + CONFIG_OVE_PROFILER_MAX_DEPTH * PC_WIRE_BYTES)
#define PROFILE_PAYLOAD_MAX (OVE_SIM_EVENT_MAX_BYTES - sizeof(struct ove_sim_event))
#define PROFILE_MAX_BATCH ((PROFILE_PAYLOAD_MAX - PROFILE_ENVELOPE_BYTES) / MAX_SAMPLE_BYTES)
#define PROFILE_BUF_BYTES (PROFILE_ENVELOPE_BYTES + PROFILE_MAX_BATCH * MAX_SAMPLE_BYTES)

_Static_assert(PROFILE_MAX_BATCH >= 1, "CONFIG_OVE_PROFILER_MAX_DEPTH too large for one event");
_Static_assert(sizeof(struct ove_sim_event) + PROFILE_BUF_BYTES <= OVE_SIM_EVENT_MAX_BYTES,
	       "profiler batch exceeds OVE_SIM_EVENT_MAX_BYTES");

/* Pump-scoped scratch: only the consolidated sim-debug pump drains the
 * ring, so these are single-threaded. Kept in BSS rather than on the pump
 * stack (they scale with CONFIG_OVE_PROFILER_MAX_DEPTH). */
static struct ove_profiler_sample drain_batch[PROFILE_MAX_BATCH];
static uint8_t __attribute__((
	aligned(8))) samples_ev_buf[sizeof(struct ove_sim_event) + PROFILE_BUF_BYTES];

static size_t serialise_sample(uint8_t *p, const struct ove_profiler_sample *s)
{
	uint8_t *q = p;
	memcpy(q, &s->ts_us, 8);
	q += 8;
	memcpy(q, &s->tid, 4);
	q += 4;
	*q++ = s->depth;
	*q++ = s->state;
	*q++ = 0;
	*q++ = 0;
	for (uint8_t i = 0; i < s->depth; i++) {
		uint64_t pc = (uint64_t)s->pcs[i];
		memcpy(q, &pc, PC_WIRE_BYTES);
		q += PC_WIRE_BYTES;
	}
	return (size_t)(q - p);
}

static void emit_batch(struct sim_profiler_ctx *t, const struct ove_profiler_sample *samples,
		       size_t n, uint32_t dropped)
{
	if (n == 0 && dropped == 0)
		return;

	struct ove_sim_event *ev = (struct ove_sim_event *)samples_ev_buf;
	uint8_t *p = ev->data;

	*p++ = OVE_SIM_PROFILER_VERSION;
	/* Wire format: PCs are always serialised as 8 bytes regardless of
	 * the target's native pointer size (see serialise_sample). Report 8
	 * so the dashboard parses 64-bit slots — otherwise a 32-bit target
	 * would report 4 and the batch gets dropped as "unsupported word size". */
	*p++ = PC_WIRE_BYTES;
	uint16_t count16 = (uint16_t)n;
	memcpy(p, &count16, 2);
	p += 2;
	memcpy(p, &dropped, 4);
	p += 4;

	for (size_t i = 0; i < n; i++)
		p += serialise_sample(p, &samples[i]);

	ev->plugin_id = t->plugin_id;
	ev->event_type = OVE_SIM_PROFILER_EVT_SAMPLES;
	ev->timestamp_ms = 0;
	ev->data_len = (uint32_t)(p - ev->data);

	ove_sim_plugin_emit_event(t->plugin_id, ev);
}

void ove_sim_profiler_tick(void)
{
	size_t n;
	do {
		n = ove_profiler_ring_drain(drain_batch, PROFILE_MAX_BATCH);
		uint32_t dropped = ove_profiler_ring_dropped_get();
		emit_batch(&profiler_ctx, drain_batch, n, dropped);
	} while (n == PROFILE_MAX_BATCH);
}

static int profiler_init(void *ctx, const void *config, size_t config_len)
{
	(void)config;
	(void)config_len;
	struct sim_profiler_ctx *t = (struct sim_profiler_ctx *)ctx;
	int ret = ove_backend_profiler_start();
	if (ret != OVE_OK) {
		fprintf(stderr, "[sim] profiler backend start failed: %d\n", ret);
		/* Continue anyway — ticks will just find an empty ring. */
	}
	/* Default to the compile-time max; dashboard may later request lower. */
	t->current_hz = ove_backend_profiler_get_max_hz();
	ove_backend_profiler_set_rate(t->current_hz);
	return OVE_OK;
}

static void profiler_deinit(void *ctx)
{
	(void)ctx;
	ove_backend_profiler_stop();
}

static int profiler_handle_cmd(void *ctx, const struct ove_sim_cmd *cmd)
{
	struct sim_profiler_ctx *t = (struct sim_profiler_ctx *)ctx;
	if (cmd->cmd_type == OVE_SIM_PROFILER_CMD_SET_RATE && cmd->data_len >= 4) {
		uint32_t hz;
		memcpy(&hz, cmd->data, 4);
		uint32_t max_hz = ove_backend_profiler_get_max_hz();
		if (hz == 0 || hz > max_hz)
			hz = max_hz;
		t->current_hz = hz;
		ove_backend_profiler_set_rate(hz);
		emit_caps(t);
	}
	return OVE_OK;
}

void ove_sim_profiler_announce_caps(void)
{
	emit_caps(&profiler_ctx);
}

static const struct ove_sim_plugin_ops profiler_ops = {
	.name = "profiler",
	.type = OVE_SIM_PLUGIN_SENSOR,
	.init = profiler_init,
	.deinit = profiler_deinit,
	.handle_cmd = profiler_handle_cmd,
};

int ove_sim_profiler_register(void)
{
	int id = ove_sim_plugin_register(&profiler_ops, &profiler_ctx, NULL, 0);
	if (id < 0)
		return id;

	profiler_ctx.plugin_id = (uint32_t)id;
	return id;
}

#else /* !CONFIG_OVE_PROFILER */

#include "ove/sim/ove_sim_profiler.h"

int ove_sim_profiler_register(void)
{
	return 0;
}
void ove_sim_profiler_tick(void)
{
}
void ove_sim_profiler_announce_caps(void)
{
}

#endif /* CONFIG_OVE_PROFILER */
