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
 * Spawns an RTOS thread that periodically snapshots thread state and
 * heap usage (~2 Hz) and emits the data as plugin events.  The
 * dashboard bridge forwards these to the browser as FRAME_THREAD
 * WebSocket frames.
 */

#include "ove/sim/ove_sim_debug.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/thread.h"
#include "ove/types.h"

#include <stdio.h>
#include <string.h>

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
	if (p + 1 > end) return 0;
	*p++ = (uint8_t)count;

	/* heap stats (4x uint32_t LE) */
	if (p + 16 > end) return 0;
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

		/* 1 + name_len + 1 + 1 + 4 + 4 + 4 = name_len + 15 */
		if (p + name_len + 15 > end)
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

/* ── Snapshot thread ──────────────────────────────────────────────── */

static void debug_thread_fn(void *arg)
{
	struct sim_debug_ctx *d = (struct sim_debug_ctx *)arg;

	for (;;) {
		ove_thread_sleep_ms(OVE_SIM_DEBUG_INTERVAL_MS);
		emit_snapshot(d);
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

static int debug_get_state(void *ctx, void *buf, size_t buf_len,
			   size_t *out_len)
{
	(void)ctx;
	int n = snprintf((char *)buf, buf_len,
			 "{\"type\":\"debug\",\"interval_ms\":%d}",
			 OVE_SIM_DEBUG_INTERVAL_MS);
	if (out_len)
		*out_len = (size_t)n;
	return OVE_OK;
}

static const struct ove_sim_plugin_ops debug_ops = {
	.name      = "debug",
	.type      = OVE_SIM_PLUGIN_SENSOR,
	.init      = debug_init,
	.deinit    = debug_deinit,
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
	 * until the scheduler is running. */
	struct ove_thread_desc desc = {
		.name     = "sim_debug",
		.entry    = debug_thread_fn,
		.arg      = &debug_ctx,
		.priority = OVE_PRIO_LOW,
	};
	int ret = ove_thread_create(&debug_thread_handle, 2048, &desc);
	if (ret != OVE_OK)
		fprintf(stderr, "[sim] debug thread create failed: %d\n", ret);

	return id;
}
