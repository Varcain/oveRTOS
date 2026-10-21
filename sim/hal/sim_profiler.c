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
#include "ove/sim/ove_sim_budget.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_profiler.h"
#include "ove/types.h"

#include "ove_profiler_ring.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct sim_profiler_ctx {
	uint32_t plugin_id;
	uint32_t current_hz;
};

static struct sim_profiler_ctx profiler_ctx;

/* Pump-thread state for events the transport refused (it never laps the
 * host, see guest_send_event()): the samples and ring drops of refused
 * batches, reported in the next batch's drop count.  Kept here rather
 * than in the ring's drop counter, which counts only the ring's own
 * overflow. */
static uint32_t profiler_refused;

/* A caps event is owed to the dashboard: the one-shot announcement, a
 * rate command, the rate guard, or a caps event the transport refused.
 * The next tick sends one, so however many rate commands arrive in one
 * pump iteration they cost a single event. */
static bool caps_pending;

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

	caps_pending = ove_sim_plugin_emit_event(t->plugin_id, ev) != OVE_OK;
}

/*
 * Envelope (1+1+2+4 = 8 B) + per sample:
 *   8 (ts) + 4 (tid) + 1 (depth) + 1 (state) + 2 (isr) + depth * W (pcs)
 *
 * W (the envelope's word_size byte) is the target's pointer size: 4 on
 * Cortex-M. Widening PCs to 8 bytes would waste half of every event.
 *
 * The state byte is ove_thread_state_t captured at sample time so the
 * dashboard can filter on-CPU vs wall-clock views.
 *
 * We serialise each sample compactly (only @depth PCs, not the full
 * fixed slot) to avoid wasting bridge bandwidth on zero-padded tails.
 *
 * A batch holds as many full-depth samples as fit in one event of at most
 * OVE_SIM_EVENT_MAX_BYTES (the host side skips larger ones), so the batch
 * size follows CONFIG_OVE_PROFILER_MAX_DEPTH (with 4-byte PCs: 28 at
 * depth 32, 14 at 64).
 */
#define PROFILE_ENVELOPE_BYTES 8u
#define SAMPLE_HDR_BYTES 16u
#define PC_WIRE_BYTES ((unsigned)sizeof(uintptr_t))
#define MAX_SAMPLE_BYTES (SAMPLE_HDR_BYTES + CONFIG_OVE_PROFILER_MAX_DEPTH * PC_WIRE_BYTES)
#define PROFILE_PAYLOAD_MAX (OVE_SIM_EVENT_MAX_BYTES - sizeof(struct ove_sim_event))
#define PROFILE_MAX_BATCH ((PROFILE_PAYLOAD_MAX - PROFILE_ENVELOPE_BYTES) / MAX_SAMPLE_BYTES)
#define PROFILE_BUF_BYTES (PROFILE_ENVELOPE_BYTES + PROFILE_MAX_BATCH * MAX_SAMPLE_BYTES)

_Static_assert(PROFILE_MAX_BATCH >= 1, "CONFIG_OVE_PROFILER_MAX_DEPTH too large for one event");
_Static_assert(sizeof(struct ove_sim_event) + PROFILE_BUF_BYTES <= OVE_SIM_EVENT_MAX_BYTES,
	       "profiler batch exceeds OVE_SIM_EVENT_MAX_BYTES");

/*
 * Per-pump-iteration event budget.
 *
 * The batches leave the guest through the shared-memory event ring
 * (SIM_SHM_RING_SIZE bytes).  One batch event occupies at most
 * PROFILE_EVENT_RING_BYTES of it: the 2-byte length prefix, the event
 * header, the envelope and a full-depth batch.  The number of batches
 * one drain may emit is therefore derived from the ring size instead
 * of being guessed, and it follows PROFILE_MAX_BATCH (hence
 * CONFIG_OVE_PROFILER_MAX_DEPTH) whenever those change.
 *
 * The ring is SHARED, though, and "half of the ring for each drain" is
 * not a budget - it is a wish.  The producers that can land in the
 * same pump iteration are
 *
 *   - this drain,
 *   - the trace drain and the trace's own descriptor and name events
 *     (sim/hal/sim_trace.c, charged to the trace's share),
 *   - the debug plugin's thread/heap snapshot (sim/hal/sim_debug.c),
 *     reserved as SIM_EVENT_SNAPSHOT_RESERVE_BYTES because that file
 *     owns the snapshot's layout.
 *
 * sim/include/ove/sim/ove_sim_budget.h owns that split, so the two
 * drains cannot drift apart: the shares add up to the ring minus the
 * snapshot reserve, and one pump iteration always fits in the ring.
 */
#define PROFILE_EVENT_RING_BYTES (SIM_EVENT_RING_OVERHEAD_BYTES + PROFILE_BUF_BYTES)

/* The capabilities event (max_hz, current_hz, flags) carries 12
 * payload bytes.  Every request for one only marks it pending
 * (caps_pending), and a tick sends at most one, so one is reserved. */
#define PROFILE_CAPS_EVENT_BYTES (SIM_EVENT_RING_OVERHEAD_BYTES + 12u)

#define PROFILE_EVENT_BUDGET_BYTES (SIM_EVENT_PROFILER_BUDGET_BYTES - PROFILE_CAPS_EVENT_BYTES)
#define PROFILE_MAX_BATCHES_PER_TICK \
	((unsigned)(PROFILE_EVENT_BUDGET_BYTES / PROFILE_EVENT_RING_BYTES))

_Static_assert(PROFILE_EVENT_BUDGET_BYTES >= PROFILE_EVENT_RING_BYTES,
	       "the event-ring budget must hold at least one profiler batch");

/*
 * Producer rate.
 *
 * The budget above is a BYTE budget; the sampler is a RATE budget.
 * This drain moves
 *
 *   PROFILE_CAPACITY_PER_DRAIN = PROFILE_MAX_BATCHES_PER_TICK * PROFILE_MAX_BATCH
 *
 * samples per drain period, i.e.
 *
 *   PROFILE_CAPACITY_PER_S = PROFILE_CAPACITY_PER_DRAIN * 1000 / OVE_SIM_PROFILER_DRAIN_MS
 *
 * samples per second - 5 600/s at the default depth of 32 with the
 * 50 ms drain period the sim-debug pump ticks at.  A sampler that
 * produces more than that fills the profiler ring faster than this
 * drain empties it: the ring overflows, its own drop counter rises and
 * the samples are destroyed - exactly the failure this budget exists
 * to remove.
 *
 * The backends produce:
 *
 *   - FreeRTOS and Zephyr: ONE sample per sampling period, so at most
 *     CONFIG_OVE_PROFILER_HZ (1 000/s) samples/s.  The budget covers
 *     that with room to spare even at the deepest stack the Kconfig
 *     allows (2 800/s at depth 64).
 *   - NuttX: ONE sample per REGISTERED THREAD per period
 *     (backends/nuttx/nuttx_profiler.c walks the whole thread
 *     registry), so hz * threads samples/s.  That is not covered by
 *     the budget: 32 threads at 1 000 Hz would be 32 000 samples/s,
 *     four times what the ring can carry.  The rate guard below
 *     lowers the requested rate when a saturated drain still sees the
 *     ring lose samples, so the rate converges to what the drain can
 *     carry and the loss stops after the first overloaded period.
 */
#define PROFILE_CAPACITY_PER_DRAIN \
	((unsigned)PROFILE_MAX_BATCHES_PER_TICK * (unsigned)PROFILE_MAX_BATCH)

/* The rate the sampler may keep running at after an overload: scaled
 * by the ratio of what the transport can carry in one drain period to
 * what the sampler produced in it. */
#define PROFILE_RATE_AFTER_OVERLOAD(hz, produced) \
	((unsigned)(hz) * (unsigned)(PROFILE_CAPACITY_PER_DRAIN) / (unsigned)(produced))

/* The transport has to carry at least CONFIG_OVE_PROFILER_HZ samples
 * per second: a backend that takes one sample per sampling period
 * (FreeRTOS, Zephyr) produces exactly that, and the drain must not be
 * the bottleneck - a backlog the drain cannot drain is a backlog the
 * profiler ring destroys. */
_Static_assert(PROFILE_CAPACITY_PER_DRAIN * 1000u / OVE_SIM_PROFILER_DRAIN_MS >=
		       CONFIG_OVE_PROFILER_HZ,
	       "the profiler drain must keep up with the configured sampling rate");

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
	memcpy(q, &s->isr, 2);
	q += 2;
	for (uint8_t i = 0; i < s->depth; i++) {
		uintptr_t pc = s->pcs[i];
		memcpy(q, &pc, PC_WIRE_BYTES);
		q += PC_WIRE_BYTES;
	}
	return (size_t)(q - p);
}

/* Returns OVE_OK, or the transport's error when the batch was not sent. */
static int emit_batch(struct sim_profiler_ctx *t, const struct ove_profiler_sample *samples,
		      size_t n, uint32_t dropped)
{
	if (n == 0 && dropped == 0)
		return OVE_OK;

	struct ove_sim_event *ev = (struct ove_sim_event *)samples_ev_buf;
	uint8_t *p = ev->data;

	*p++ = OVE_SIM_PROFILER_VERSION;
	*p++ = (uint8_t)PC_WIRE_BYTES; /* word_size: bytes per PC */
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

	return ove_sim_plugin_emit_event(t->plugin_id, ev);
}

/*
 * Rate guard.
 *
 * The sampler must not produce more samples than the drain can carry:
 * the profiler ring would overflow, its own drop counter would rise
 * and the samples would be destroyed - the failure this budget exists
 * to remove.  The plugin cannot ask the backend how many samples it
 * takes per period (the NuttX wall-clock backend walks EVERY
 * registered thread once per period), so the guard measures instead:
 * the drain knows how many samples it carried, and the ring knows how
 * many were lost.
 *
 * A loss while the drain was NOT saturated is a transient (a race in
 * the sampler, a skipped sampling period) and not a rate problem, so
 * it is left alone; the rate is only scaled when the drain ran at
 * full capacity and the ring still lost samples.
 */
static void profiler_rate_guard(uint32_t delivered, uint32_t lost)
{
	if (lost == 0u || delivered < PROFILE_CAPACITY_PER_DRAIN)
		return;

	uint32_t hz = profiler_ctx.current_hz;
	if (hz <= 1u)
		return;

	uint32_t produced = delivered + lost;
	uint32_t new_hz = PROFILE_RATE_AFTER_OVERLOAD(hz, produced);
	if (new_hz < 1u)
		new_hz = 1u;
	if (new_hz >= hz)
		return;

	profiler_ctx.current_hz = new_hz;
	ove_backend_profiler_set_rate(new_hz);
	/* The dashboard shows the selected rate: tell it on the next tick. */
	caps_pending = true;
}

void ove_sim_profiler_tick(void)
{
	size_t n;
	unsigned batches = 0;
	uint32_t delivered = 0;
	uint32_t lost = 0;

	if (caps_pending)
		emit_caps(&profiler_ctx);
	do {
		n = ove_profiler_ring_drain(drain_batch, PROFILE_MAX_BATCH);
		uint32_t ring_dropped = ove_profiler_ring_dropped_get();
		uint32_t dropped = ring_dropped + profiler_refused;
		profiler_refused = 0;
		/* The rate guard sees the ring's own overflow and what reached
		 * the host: a batch the transport refused (the host is behind)
		 * is not a sampling-rate problem.  Its samples and the drops it
		 * reported are carried into the next batch's count. */
		lost += ring_dropped;
		if (emit_batch(&profiler_ctx, drain_batch, n, dropped) == OVE_OK)
			delivered += (uint32_t)n;
		else
			profiler_refused = dropped + (uint32_t)n;
		batches++;
	} while (n == PROFILE_MAX_BATCH && batches < PROFILE_MAX_BATCHES_PER_TICK);

	/* Keep the sampler inside the transport's capacity: a backlog that
	 * grows because the sampler outruns the drain is lost to the ring's
	 * overflow, not deferred. */
	profiler_rate_guard(delivered, lost);
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
		caps_pending = true;
	}
	return OVE_OK;
}

void ove_sim_profiler_announce_caps(void)
{
	caps_pending = true;
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
