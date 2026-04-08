/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Sim audio device plugin -- provides sim audio device nodes.
 *
 * Provides audio graph source/sink nodes that stream PCM data to/from
 * the web dashboard.
 */

#include "ove/types.h"
#include "ove/audio_device.h"

#ifdef CONFIG_OVE_AUDIO

#include "ove/sim/ove_sim_audio.h"
#include "ove/sim/ove_sim_plugin.h"
#include "ove/sim/ove_sim_transport.h"
#include "ove_sim_audio_ring.h"
#ifdef __EMSCRIPTEN__
#include "ove_sim_wasm_audio.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Audio plugin context ──────────────────────────────────────────── */

struct sim_audio_ctx {
	struct ove_sim_audio_cfg cfg;
	uint32_t                 plugin_id;

	/* Input ring: dashboard -> firmware (for capture). */
	struct ove_sim_audio_ring input_ring;
};

static struct sim_audio_ctx audio_ctx;

/* ── Plugin ops ────────────────────────────────────────────────────── */

static int audio_init(void *ctx, const void *config, size_t config_len)
{
	struct sim_audio_ctx *a = (struct sim_audio_ctx *)ctx;
	if (config && config_len >= sizeof(struct ove_sim_audio_cfg))
		memcpy(&a->cfg, config, sizeof(a->cfg));
	ove_sim_audio_ring_init(&a->input_ring,
				a->cfg.fmt.sample_rate,
				a->cfg.fmt.channels,
				a->cfg.fmt.bit_depth);
	return OVE_OK;
}

static int audio_handle_cmd(void *ctx, const struct ove_sim_cmd *cmd)
{
	struct sim_audio_ctx *a = (struct sim_audio_ctx *)ctx;

	if (cmd->cmd_type == OVE_SIM_AUDIO_CMD_INJECT && cmd->data_len > 0) {
#ifdef __EMSCRIPTEN__
		ove_sim_ring_write_atomic(&a->input_ring, cmd->data,
					  cmd->data_len);
#else
		ove_sim_ring_write(&a->input_ring, cmd->data,
				   cmd->data_len);
#endif
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

/* ── Push/pull PCM API (platform-agnostic via transport) ───────────── */

void ove_sim_audio_push_output(const void *samples, size_t len,
			       const struct ove_sim_audio_fmt *fmt)
{
	struct ove_sim_transport *t = ove_sim_get_transport();
	ove_sim_transport_push_audio(t, samples, len,
				     fmt->sample_rate, fmt->channels,
				     fmt->bit_depth);
}

size_t ove_sim_audio_pull_input(void *samples, size_t len,
				const struct ove_sim_audio_fmt *fmt)
{
	(void)fmt;

	/* Try transport first (WASM / SHM guest). */
	struct ove_sim_transport *t = ove_sim_get_transport();
	size_t got = ove_sim_transport_pull_audio(t, samples, len);
	if (got > 0) {
		if (got < len)
			memset((uint8_t *)samples + got, 0, len - got);
		return got;
	}

	/* Fallback: local input ring (filled by plugin CMD_INJECT from WS). */
	struct sim_audio_ctx *a = &audio_ctx;
#ifdef __EMSCRIPTEN__
	uint32_t nr = ove_sim_ring_read_atomic(&a->input_ring, samples, (uint32_t)len);
#else
	uint32_t nr = ove_sim_ring_read(&a->input_ring, samples, (uint32_t)len);
#endif
	if (nr < len)
		memset((uint8_t *)samples + nr, 0, len - nr);
	return nr;
}

/* ═══════════════════════════════════════════════════════════════════
   Sim Audio Graph Source/Sink Nodes (replace sim nodes)
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

#include "ove/thread.h"
#include "ove/time.h"

struct sim_sink_ctx {
	struct ove_audio_fmt     fmt;
	struct ove_sim_audio_fmt sim_fmt;
	struct ove_audio_graph  *graph;
	unsigned int             frames_per_period;
	ove_thread_t             pump_thread;
	volatile int             running;
};

static void sim_sink_pump(void *arg)
{
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)arg;
	uint32_t period_ms = (sc->frames_per_period * 1000) /
			     sc->fmt.sample_rate;
	if (period_ms < 1) period_ms = 1;

#ifdef __EMSCRIPTEN__
	/* WASM: consumer-driven pacing.  Produce until the playback ring
	 * is full enough, then yield and let the AudioContext consume.
	 * This makes the browser's audio clock the timing master. */
	struct ove_sim_audio_ring *pbr = &ove_wasm_audio.playback;
	uint32_t target = pbr->size * 3 / 4;

	while (sc->running) {
		uint32_t avail = ove_sim_ring_avail_atomic(pbr);
		if (avail < target) {
			ove_audio_graph_process(sc->graph);
		} else {
			ove_thread_sleep_ms(1);
		}
	}
#else
	while (sc->running) {
		ove_audio_graph_process(sc->graph);
		ove_thread_sleep_ms(period_ms);
	}
#endif
}

static int sim_sink_start(void *ctx)
{
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)ctx;
	sc->running = 1;
	struct ove_thread_desc desc = {
		.name     = "audio-pump",
		.entry    = sim_sink_pump,
		.arg      = sc,
		.priority = OVE_PRIO_HIGH,
	};
	return ove_thread_create(&sc->pump_thread, 4096, &desc);
}

static int sim_sink_stop(void *ctx)
{
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)ctx;
	sc->running = 0;
	/* Thread will exit on next sleep; give it time. */
	ove_thread_sleep_ms(50);
	return OVE_OK;
}

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
	struct sim_sink_ctx *sc = (struct sim_sink_ctx *)ctx;
	if (sc->running)
		sim_sink_stop(ctx);
	free(ctx);
}

static const struct ove_audio_node_ops sim_sink_ops = {
	.configure = sim_sink_configure,
	.process   = sim_sink_process,
	.start     = sim_sink_start,
	.stop      = sim_sink_stop,
	.destroy   = sim_sink_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
   Device Node Factories (replace sim factories in posix_audio.c)
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

	/* Accept both sim and I2S transport -- sim handles both. */

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
