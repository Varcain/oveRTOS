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
#include "ove/types.h"

#include <stdio.h>
#include <string.h>

struct ove_sim_transport *ove_sim_get_transport(void);

#ifndef CONFIG_OVE_PROFILER_HZ
#define CONFIG_OVE_PROFILER_HZ 250
#endif

/* ── Plugin context ────────────────────────────────────────────────── */

struct sim_debug_ctx {
	uint32_t plugin_id;
};

static struct sim_debug_ctx debug_ctx;

/* ── Snapshot serialisation ────────────────────────────────────────── */

/* Maximum binary snapshot size:
 *   1 (count) + 16 (mem) + 16 * (1+16+1+1+4+4) = 17 + 16*27 = 449 bytes */
#define SNAPSHOT_BUF_SIZE 512

static size_t build_snapshot(uint8_t *buf, size_t buf_size)
{
	struct ove_thread_info threads[OVE_SIM_DEBUG_MAX_THREADS];
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

		/* 1 + name_len + 1 + 1 + 4 + 4 + 4 + 16 = name_len + 31 */
		if (p + name_len + 31 > end)
			break;

		*p++ = (uint8_t)name_len;
		memcpy(p, name, name_len);
		p += name_len;

		*p++ = (uint8_t)threads[i].state;
		*p++ = (uint8_t)threads[i].priority;

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
		if (total_us > 0) {
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
	uint8_t payload[SNAPSHOT_BUF_SIZE];
	size_t payload_len = build_snapshot(payload, sizeof(payload));
	if (payload_len == 0)
		return;

	uint8_t ev_buf[sizeof(struct ove_sim_event) + SNAPSHOT_BUF_SIZE];
	struct ove_sim_event *ev = (struct ove_sim_event *)ev_buf;
	ev->plugin_id = d->plugin_id;
	ev->event_type = OVE_SIM_DEBUG_EVT_THREADS;
	ev->timestamp_ms = 0;
	ev->data_len = (uint32_t)payload_len;
	memcpy(ev->data, payload, payload_len);

	ove_sim_plugin_emit_event(d->plugin_id, ev);
}

/* ── Consolidated pump thread ─────────────────────────────────────── */

/*
 * The pump tick is set to the profiler sampling period when profiling
 * is enabled (4 ms at the default 250 Hz) so sample cadence is
 * preserved. When profiling is off we fall back to the trace drain
 * period, or the snapshot period as a last resort.
 */
#if defined(CONFIG_OVE_PROFILER)
#if (1000 / CONFIG_OVE_PROFILER_HZ) < 1
#define PUMP_TICK_MS 1
#else
#define PUMP_TICK_MS (1000 / CONFIG_OVE_PROFILER_HZ)
#endif
#elif defined(CONFIG_OVE_TRACE_STREAM)
#define PUMP_TICK_MS OVE_SIM_TRACE_DRAIN_MS
#else
#define PUMP_TICK_MS OVE_SIM_DEBUG_INTERVAL_MS
#endif

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

		struct ove_sim_transport *tr = ove_sim_get_transport();
		drain_commands(tr);

#ifdef CONFIG_OVE_PROFILER
		if (!caps_announced && tr) {
			ove_sim_profiler_announce_caps();
			caps_announced = 1;
		}

		/* Fast path: fire the sampling signal every tick. */
		ove_backend_profiler_sample_tick();

		t_prof += PUMP_TICK_MS;
		if (t_prof >= OVE_SIM_PROFILER_DRAIN_MS) {
			t_prof = 0;
			ove_sim_profiler_tick();
		}
#endif

#ifdef CONFIG_OVE_TRACE_STREAM
		t_trace += PUMP_TICK_MS;
		if (t_trace >= OVE_SIM_TRACE_DRAIN_MS) {
			ove_sim_trace_tick(t_trace);
			t_trace = 0;
		}
#endif

		t_snap += PUMP_TICK_MS;
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
	 * Stack sized for the worst-case drain-path frame. sim_profiler_tick
	 * holds batch[24] (~2 KB) while calling emit_batch, which itself
	 * stack-allocates payload + ev_buf (~7 KB wire-size × 2). 16 KB
	 * leaves a generous margin; POSIX's default pthread stack handled
	 * this silently, FreeRTOS honours the request literally. */
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
