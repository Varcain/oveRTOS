/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Debug simulation plugin.
 *
 * Owns the single "sim-debug" pump thread that drives all sim-side
 * observability work: profiler sample tick (fast), profiler-ring drain,
 * trace-ring drain, and periodic thread/heap snapshots. Previously each
 * subsystem spawned its own thread (4 in total); consolidating them
 * into one pump cuts context-switch pressure on the RTOS scheduler
 * without changing the per-feature cadence.
 */

#include "ove_config.h"

#include "ove/profiler.h"
#include "ove/sim/ove_sim_debug.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_profiler.h"
#include "ove/sim/ove_sim_trace.h"
#include "ove/sim/ove_sim_transport.h"
#include "ove/thread.h"
#include "ove/time.h"
#include "ove/types.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct ove_sim_transport *ove_sim_get_transport(void);

/* ── Plugin context ────────────────────────────────────────────────── */

struct sim_debug_ctx {
	uint32_t plugin_id;
};

static struct sim_debug_ctx debug_ctx;

/* ── Snapshot serialisation ────────────────────────────────────────── */

/* Maximum binary snapshot size:
 *   1 (count) + 16 (mem) + MAX_THREADS * (1 + name + 1+1+4+4+4+4+16) */
#define SNAPSHOT_ENTRY_MAX (OVE_SIM_DEBUG_MAX_NAME_LEN + 35u)
#define SNAPSHOT_BUF_SIZE (17u + OVE_SIM_DEBUG_MAX_THREADS * SNAPSHOT_ENTRY_MAX)

_Static_assert(sizeof(struct ove_sim_event) + SNAPSHOT_BUF_SIZE <= OVE_SIM_EVENT_MAX_BYTES,
	       "thread snapshot exceeds OVE_SIM_EVENT_MAX_BYTES");

/* Pump-scoped scratch (single caller), kept off the pump stack. */
static struct ove_thread_info snap_threads[OVE_SIM_DEBUG_MAX_THREADS];
static uint8_t
	__attribute__((aligned(8))) snap_ev_buf[sizeof(struct ove_sim_event) + SNAPSHOT_BUF_SIZE];

static size_t build_snapshot(uint8_t *buf, size_t buf_size)
{
	struct ove_thread_info *threads = snap_threads;
	size_t count = 0;

	ove_thread_list(threads, OVE_SIM_DEBUG_MAX_THREADS, &count);

	struct ove_mem_stats mem;
	memset(&mem, 0, sizeof(mem));
	ove_sys_get_mem_stats(&mem);

	uint8_t *p = buf;
	uint8_t *end = buf + buf_size;

	/* thread_count (uint8_t) */
	if (p + 1 > end)
		return 0;
	*p++ = (uint8_t)count;

	/* heap stats (4x uint32_t LE) */
	if (p + 16 > end)
		return 0;
	uint32_t vals[4] = {
		(uint32_t)mem.total,
		(uint32_t)mem.free,
		(uint32_t)mem.used,
		(uint32_t)mem.peak_used,
	};
	memcpy(p, vals, 16);
	p += 16;

	/* per-thread entries */
	for (size_t i = 0; i < count; i++) {
		const char *name = threads[i].name ? threads[i].name : "?";
		size_t name_len = strlen(name);
		if (name_len > OVE_SIM_DEBUG_MAX_NAME_LEN)
			name_len = OVE_SIM_DEBUG_MAX_NAME_LEN;

		/* 1 + name_len + 1 + 1 + 4 + 4 + 4 + 4 + 16 = name_len + 35 */
		if (p + name_len + 35 > end)
			break;

		*p++ = (uint8_t)name_len;
		memcpy(p, name, name_len);
		p += name_len;

		*p++ = (uint8_t)threads[i].state;
		*p++ = (uint8_t)threads[i].priority;

		memcpy(p, &threads[i].valid_fields, 4);
		p += 4;

		uint32_t stack_used = (uint32_t)threads[i].stack_used;
		memcpy(p, &stack_used, 4);
		p += 4;

		uint32_t stack_size = (uint32_t)threads[i].stack_size;
		memcpy(p, &stack_size, 4);
		p += 4;

		uint32_t cpu_x100 = threads[i].cpu_percent_x100;
		memcpy(p, &cpu_x100, 4);
		p += 4;

		/* Per-state time percentages (x100). */
		struct ove_thread_state_times *st = &threads[i].state_times;
		uint64_t total_us =
			st->running_us + st->ready_us + st->blocked_us + st->suspended_us;
		uint32_t st_pct[4] = {0, 0, 0, 0};
		if (ove_thread_info_has(&threads[i], OVE_THREAD_INFO_VALID_STATE_TIMES) &&
		    total_us > 0) {
			st_pct[0] = (uint32_t)(st->running_us * 10000U / total_us);
			st_pct[1] = (uint32_t)(st->ready_us * 10000U / total_us);
			st_pct[2] = (uint32_t)(st->blocked_us * 10000U / total_us);
			st_pct[3] = (uint32_t)(st->suspended_us * 10000U / total_us);
		}
		memcpy(p, st_pct, 16);
		p += 16;
	}

	return (size_t)(p - buf);
}

static void emit_snapshot(struct sim_debug_ctx *d)
{
	struct ove_sim_event *ev = (struct ove_sim_event *)snap_ev_buf;
	size_t payload_len = build_snapshot(ev->data, SNAPSHOT_BUF_SIZE);
	if (payload_len == 0)
		return;

	ev->plugin_id = d->plugin_id;
	ev->event_type = OVE_SIM_DEBUG_EVT_THREADS;
	ev->timestamp_ms = 0;
	ev->data_len = (uint32_t)payload_len;

	ove_sim_plugin_emit_event(d->plugin_id, ev);
}

/* ── Consolidated pump thread ─────────────────────────────────────── */

/*
 * The pump only drains and reports — sampling runs on its own in every
 * profiler backend (tick hook, timer interrupt, timer-queued work). Its
 * tick just bounds dashboard command latency; the drains keep their own
 * cadence (trace 100 ms, profiler 200 ms, snapshots 500 ms), scheduled
 * from measured time so a late or starved pump drains once on its next
 * pass instead of stretching every interval.
 */
#define PUMP_TICK_MS 50u

/* Elapsed milliseconds since the previous call (measured when the time
 * service is available, else the nominal tick). */
static uint32_t pump_elapsed_ms(uint64_t *last_us)
{
	uint64_t now_us;
	if (ove_time_get_us(&now_us) != OVE_OK)
		return PUMP_TICK_MS;
	uint64_t prev = *last_us;
	*last_us = now_us;
	if (prev == 0 || now_us < prev)
		return PUMP_TICK_MS;
	uint64_t ms = (now_us - prev) / 1000u;
	return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

/*
 * Small buffer sized for our current in-tree commands (all payloads fit
 * comfortably in 64 bytes). Oversize messages are silently dropped by
 * the transport rather than truncating into this buffer.
 */
#define CMD_BUF_BYTES (sizeof(struct ove_sim_cmd) + 64)

static void drain_commands(struct ove_sim_transport *tr)
{
	if (!tr)
		return;
	uint8_t buf[CMD_BUF_BYTES];
	struct ove_sim_cmd *cmd = (struct ove_sim_cmd *)buf;
	while (ove_sim_transport_recv_cmd(tr, cmd, sizeof(buf), 0) == OVE_OK)
		ove_sim_plugin_dispatch_cmd(cmd);
}

static void debug_thread_fn(void *arg)
{
	struct sim_debug_ctx *d = (struct sim_debug_ctx *)arg;

	uint64_t last_us = 0;
	uint32_t t_snap = 0;
#ifdef CONFIG_OVE_TRACE_STREAM
	uint32_t t_trace = 0;
#endif
#ifdef CONFIG_OVE_PROFILER
	uint32_t t_prof = 0;
	/* Announce profiler caps once so the dashboard can populate its
	 * rate dropdown with the compile-time max. Done from the pump so it
	 * happens after the transport is live. */
	int caps_announced = 0;
#endif

	for (;;) {
		ove_thread_sleep_ms(PUMP_TICK_MS);
		uint32_t elapsed = pump_elapsed_ms(&last_us);

		struct ove_sim_transport *tr = ove_sim_get_transport();
		drain_commands(tr);

#ifdef CONFIG_OVE_PROFILER
		if (!caps_announced && tr) {
			ove_sim_profiler_announce_caps();
			caps_announced = 1;
		}

		t_prof += elapsed;
		if (t_prof >= OVE_SIM_PROFILER_DRAIN_MS) {
			t_prof = 0;
			ove_sim_profiler_tick();
		}
#endif

#ifdef CONFIG_OVE_TRACE_STREAM
		t_trace += elapsed;
		if (t_trace >= OVE_SIM_TRACE_DRAIN_MS) {
			ove_sim_trace_tick(t_trace);
			t_trace = 0;
		}
#endif

		t_snap += elapsed;
		if (t_snap >= OVE_SIM_DEBUG_INTERVAL_MS) {
			t_snap = 0;
			emit_snapshot(d);
		}
	}
}

static ove_thread_t debug_thread_handle;

/* ── Plugin ops ────────────────────────────────────────────────────── */

static int debug_init(void *ctx, const void *config, size_t config_len)
{
	(void)config;
	(void)config_len;
	(void)ctx;
	return OVE_OK;
}

static void debug_deinit(void *ctx)
{
	(void)ctx;
}

static int debug_get_state(void *ctx, void *buf, size_t buf_len, size_t *out_len)
{
	(void)ctx;
	int n = snprintf((char *)buf, buf_len, "{\"type\":\"debug\",\"interval_ms\":%d}",
			 OVE_SIM_DEBUG_INTERVAL_MS);
	if (out_len)
		*out_len = (size_t)n;
	return OVE_OK;
}

static const struct ove_sim_plugin_ops debug_ops = {
	.name = "debug",
	.type = OVE_SIM_PLUGIN_SENSOR,
	.init = debug_init,
	.deinit = debug_deinit,
	.get_state = debug_get_state,
};

/* ── Registration helper ───────────────────────────────────────────── */

int ove_sim_debug_register(void)
{
	int id = ove_sim_plugin_register(&debug_ops, &debug_ctx, NULL, 0);
	if (id < 0)
		return id;

	debug_ctx.plugin_id = (uint32_t)id;

	/* Spawn a low-priority thread that emits snapshots every 500 ms.
	 * The thread starts immediately but ove_thread_sleep_ms will block
	 * until the scheduler is running.
	 *
	 * The profiler, trace and thread-snapshot buffers live in BSS; the
	 * deepest frame left on this stack is the profiler symbol drain
	 * (~4 KB). 16 KB leaves a generous margin; FreeRTOS honours the
	 * request literally. */
#ifdef OVE_HEAP_THREAD
	int ret = ove_thread_create(&debug_thread_handle, "sim_debug", debug_thread_fn, &debug_ctx,
				    OVE_PRIO_LOW, 16384);
	if (ret != OVE_OK)
		fprintf(stderr, "[sim] debug thread create failed: %d\n", ret);
#else
	static ove_thread_storage_t debug_th_storage;
	static uint8_t __attribute__((aligned(8))) debug_th_stack[16384];
	int ret = ove_thread_init(&debug_thread_handle, &debug_th_storage, "sim_debug",
				  debug_thread_fn, &debug_ctx, OVE_PRIO_LOW, sizeof(debug_th_stack),
				  debug_th_stack);
	if (ret != OVE_OK)
		fprintf(stderr, "[sim] debug thread init failed: %d\n", ret);
#endif

	return id;
}
