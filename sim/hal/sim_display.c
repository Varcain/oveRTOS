/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Display simulation plugin.
 *
 * Receives framebuffer flushes from sim_lvgl.c and emits them as
 * events through the sim transport.  The web dashboard renders these
 * into an HTML5 <canvas>.
 */

#include "ove/sim/ove_sim_display.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/types.h"

#include <stdlib.h>
#include <string.h>

/* ── Plugin context ────────────────────────────────────────────────── */

struct sim_display_ctx {
	struct ove_sim_display_cfg cfg;
	uint32_t plugin_id;
	uint32_t frame_count;
};

static struct sim_display_ctx display_ctx;

/* ── Plugin ops ────────────────────────────────────────────────────── */

static int display_init(void *ctx, const void *config, size_t config_len)
{
	struct sim_display_ctx *d = (struct sim_display_ctx *)ctx;
	if (config && config_len >= sizeof(struct ove_sim_display_cfg))
		memcpy(&d->cfg, config, sizeof(d->cfg));
	d->frame_count = 0;
	return OVE_OK;
}

static const struct ove_sim_display_ops builtin_display_ops = {
	.base =
		{
			.init = display_init,
		},
};

/* ── Flush notification (called from sim_lvgl.c) ──────────────────── */

void ove_sim_display_flush(const void *fb, size_t fb_len, uint16_t x1, uint16_t y1, uint16_t x2,
			   uint16_t y2)
{
	display_ctx.frame_count++;

	/* Emit event through the transport (for QEMU mode). */
	size_t hdr_size = sizeof(struct ove_sim_event) + 8; /* 8 = coords */
	size_t total = hdr_size + fb_len;
	uint8_t *buf = malloc(total);
	if (!buf)
		return;

	struct ove_sim_event *ev = (struct ove_sim_event *)buf;
	ev->plugin_id = display_ctx.plugin_id;
	ev->event_type = OVE_SIM_DISPLAY_EVT_FRAME;
	ev->timestamp_ms = display_ctx.frame_count; /* simple counter */
	ev->data_len = (uint32_t)(8 + fb_len);

	uint16_t coords[4] = {x1, y1, x2, y2};
	memcpy(ev->data, coords, 8);
	memcpy(ev->data + 8, fb, fb_len);

	ove_sim_plugin_emit_event(display_ctx.plugin_id, ev);
	free(buf);
}

/* ── Registration helper ───────────────────────────────────────────── */

int ove_sim_display_register(uint16_t width, uint16_t height, enum ove_sim_color_fmt fmt)
{
	display_ctx.cfg.width = width;
	display_ctx.cfg.height = height;
	display_ctx.cfg.color_fmt = fmt;

	int id = ove_sim_plugin_register(&builtin_display_ops.base, &display_ctx, &display_ctx.cfg,
					 sizeof(display_ctx.cfg));
	if (id >= 0)
		display_ctx.plugin_id = (uint32_t)id;

	return id;
}
