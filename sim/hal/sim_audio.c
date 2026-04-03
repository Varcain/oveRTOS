/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim audio device plugin -- replaces posix_audio.c (SDL2-based).
 *
 * Provides audio graph source/sink nodes that stream PCM data to/from
 * the web dashboard instead of SDL2.
 */

#include "ove/types.h"
#include "ove/audio_device.h"

#ifdef CONFIG_OVE_AUDIO

#include "ove/sim/ove_sim_audio.h"
#include "ove/sim/ove_sim_plugin.h"
#include "../src/ove_sim_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Audio plugin context ──────────────────────────────────────────── */

struct sim_audio_ctx {
	struct ove_sim_audio_cfg cfg;
	uint32_t                 plugin_id;

	/* Input ring: dashboard -> firmware (for capture). */
	uint8_t                  input_ring[32768];
	uint32_t                 input_write;
	uint32_t                 input_read;
};

static struct sim_audio_ctx audio_ctx;

/* ── Plugin ops ────────────────────────────────────────────────────── */

static int audio_init(void *ctx, const void *config, size_t config_len)
{
	struct sim_audio_ctx *a = (struct sim_audio_ctx *)ctx;
	if (config && config_len >= sizeof(struct ove_sim_audio_cfg))
		memcpy(&a->cfg, config, sizeof(a->cfg));
	a->input_write = 0;
	a->input_read = 0;
	return OVE_OK;
}

static int audio_handle_cmd(void *ctx, const struct ove_sim_cmd *cmd)
{
	struct sim_audio_ctx *a = (struct sim_audio_ctx *)ctx;

	if (cmd->cmd_type == OVE_SIM_AUDIO_CMD_INJECT && cmd->data_len > 0) {
		/* Write incoming PCM into the input ring. */
		uint32_t ring_mask = sizeof(a->input_ring) - 1;
		for (uint32_t i = 0; i < cmd->data_len; i++) {
			a->input_ring[a->input_write & ring_mask] =
				cmd->data[i];
			a->input_write++;
		}
	}
	return OVE_OK;
}

static int audio_get_state(void *ctx, void *buf, size_t buf_len,
			   size_t *out_len)
{
	struct sim_audio_ctx *a = (struct sim_audio_ctx *)ctx;
	int n = snprintf((char *)buf, buf_len,
			 "{\"type\":\"audio\",\"sample_rate\":%u,"
			 "\"channels\":%u,\"bit_depth\":%u}",
			 a->cfg.fmt.sample_rate,
			 a->cfg.fmt.channels,
			 a->cfg.fmt.bit_depth);
	if (out_len)
		*out_len = (size_t)n;
	return OVE_OK;
}

static const struct ove_sim_plugin_ops sim_audio_ops = {
	.name       = "audio",
	.type       = OVE_SIM_PLUGIN_AUDIO,
	.init       = audio_init,
	.handle_cmd = audio_handle_cmd,
	.get_state  = audio_get_state,
};

const struct ove_sim_plugin_ops *ove_sim_audio_builtin_ops(void)
{
	return &sim_audio_ops;
}

/* ── Push/pull PCM API ─────────────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
#include "../src/ove_sim_wasm_audio.h"
#endif

void ove_sim_audio_push_output(const void *samples, size_t len,
			       const struct ove_sim_audio_fmt *fmt)
{
#ifdef __EMSCRIPTEN__
	/* WASM: write to shared ring buffer → JS AudioWorklet → speaker */
	ove_wasm_audio_playback_write(samples, (uint32_t)len);
	(void)fmt;
#else
	/* POSIX: stream to dashboard via WebSocket */
	if (!ove_sim_ws_has_clients())
		return;

	size_t hdr = 8;
	size_t total = hdr + len;
	uint8_t *frame = malloc(total);
	if (!frame)
		return;

	memcpy(frame, &fmt->sample_rate, 4);
	memcpy(frame + 4, &fmt->channels, 2);
	memcpy(frame + 6, &fmt->bit_depth, 2);
	memcpy(frame + 8, samples, len);

	ove_sim_ws_broadcast(OVE_SIM_WS_FRAME_AUDIO, frame, total);
	free(frame);
#endif
}

size_t ove_sim_audio_pull_input(void *samples, size_t len,
				const struct ove_sim_audio_fmt *fmt)
{
#ifdef __EMSCRIPTEN__
	/* WASM: read from shared ring buffer ← JS AudioWorklet ← mic */
	(void)fmt;
	return ove_wasm_audio_capture_read(samples, (uint32_t)len);
#else
	(void)fmt;
	struct sim_audio_ctx *a = &audio_ctx;
	uint32_t avail = a->input_write - a->input_read;
	if (avail == 0) {
		memset(samples, 0, len);
		return 0;
	}

	uint32_t to_read = avail < (uint32_t)len ? avail : (uint32_t)len;
	uint32_t ring_mask = sizeof(a->input_ring) - 1;
	uint8_t *dst = (uint8_t *)samples;

	for (uint32_t i = 0; i < to_read; i++) {
		dst[i] = a->input_ring[a->input_read & ring_mask];
		a->input_read++;
	}

	if (to_read < len)
		memset(dst + to_read, 0, len - to_read);

	return to_read;
#endif
}

/* ═══════════════════════════════════════════════════════════════════
   Sim Audio Graph Source/Sink Nodes (replace SDL2 nodes)
   ═══════════════════════════════════════════════════════════════════ */

#include "ove/audio.h"

/* ── Source node (capture from dashboard) ──────────────────────────── */

struct sim_source_ctx {
	struct ove_audio_fmt     fmt;
	struct ove_sim_audio_fmt sim_fmt;
};

static int sim_source_configure(void *ctx, const struct ove_audio_fmt *in,
				struct ove_audio_fmt *out)
{
	(void)in;
	struct sim_source_ctx *sc = (struct sim_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int sim_source_process(void *ctx, const struct ove_audio_buf *in,
			      struct ove_audio_buf *out)
{
	(void)in;
	struct sim_source_ctx *sc = (struct sim_source_ctx *)ctx;
	unsigned int bytes = out->frames * out->fmt->channels *
			     ove_audio_sample_size(out->fmt->sample_fmt);
	ove_sim_audio_pull_input(out->data, bytes, &sc->sim_fmt);
	return OVE_OK;
}

static void sim_source_destroy(void *ctx)
{
	free(ctx);
}

static const struct ove_audio_node_ops sim_source_ops = {
	.configure = sim_source_configure,
	.process   = sim_source_process,
	.destroy   = sim_source_destroy,
};

/* ── Sink node (playback to dashboard) ─────────────────────────────── */

struct sim_sink_ctx {
	struct ove_audio_fmt     fmt;
	struct ove_sim_audio_fmt sim_fmt;
	struct ove_audio_graph  *graph;
	unsigned int             frames_per_period;
};

static int sim_sink_configure(void *ctx, const struct ove_audio_fmt *in,
			      struct ove_audio_fmt *out)
{
	(void)out;
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt))
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int sim_sink_process(void *ctx, const struct ove_audio_buf *in,
			    struct ove_audio_buf *out)
{
	(void)out;
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)ctx;
	if (in && in->data) {
		unsigned int bytes = in->frames * in->fmt->channels *
				     ove_audio_sample_size(in->fmt->sample_fmt);
		ove_sim_audio_push_output(in->data, bytes, &sc->sim_fmt);
	}
	return OVE_OK;
}

static void sim_sink_destroy(void *ctx)
{
	free(ctx);
}

static const struct ove_audio_node_ops sim_sink_ops = {
	.configure = sim_sink_configure,
	.process   = sim_sink_process,
	.destroy   = sim_sink_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
   Device Node Factories (replace SDL2 factories in posix_audio.c)
   ═══════════════════════════════════════════════════════════════════ */

static struct ove_sim_audio_fmt fmt_from_ove(const struct ove_audio_fmt *f)
{
	struct ove_sim_audio_fmt sf;
	sf.sample_rate = f->sample_rate;
	sf.channels = f->channels;
	sf.bit_depth = (uint16_t)(ove_audio_sample_size(f->sample_fmt) * 8);
	return sf;
}

int ove_audio_device_source(struct ove_audio_graph *g,
			    const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

	/* Accept both SDL2 and I2S transport -- sim handles both. */

	struct sim_source_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;

	ctx->fmt = cfg->fmt;
	ctx->sim_fmt = fmt_from_ove(&cfg->fmt);

	int idx = ove_audio_graph_add_node(g, &sim_source_ops, ctx, name,
					   OVE_AUDIO_NODE_SOURCE);
	if (idx < 0)
		free(ctx);
	return idx;
}

int ove_audio_device_sink(struct ove_audio_graph *g,
			  const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

	struct sim_sink_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;

	ctx->fmt = cfg->fmt;
	ctx->sim_fmt = fmt_from_ove(&cfg->fmt);
	ctx->graph = g;
	ctx->frames_per_period = g->frames_per_period;

	int idx = ove_audio_graph_add_node(g, &sim_sink_ops, ctx, name,
					   OVE_AUDIO_NODE_SINK);
	if (idx < 0)
		free(ctx);
	return idx;
}

/* ── Registration helper ───────────────────────────────────────────── */

int ove_sim_audio_register(uint32_t sample_rate, uint16_t channels,
			   uint16_t bit_depth, uint32_t buffer_frames)
{
	audio_ctx.cfg.fmt.sample_rate = sample_rate;
	audio_ctx.cfg.fmt.channels = channels;
	audio_ctx.cfg.fmt.bit_depth = bit_depth;
	audio_ctx.cfg.buffer_frames = buffer_frames;

	int id = ove_sim_plugin_register(&sim_audio_ops, &audio_ctx,
					 &audio_ctx.cfg,
					 sizeof(audio_ctx.cfg));
	if (id >= 0)
		audio_ctx.plugin_id = (uint32_t)id;

	return id;
}

#endif /* CONFIG_OVE_AUDIO */
