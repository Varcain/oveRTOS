/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU audio driver — streams PCM through /dev/shm/ove-audio via ARM
 * semihosting.  The host-side viewer (ove-dashboard-bridge.py) mmaps
 * the same file for audio playback and capture.
 *
 * Falls back to silent in-memory processing when the shared-memory
 * file cannot be opened (headless / no audio viewer).
 *
 * Uses the common ove_sim_audio_ring layout for the SHM file so all
 * sim transports share the same ring struct.
 *
 * Implements ove_audio_device_source() / ove_audio_device_sink() as
 * graph device nodes. The sink's start() creates the engine thread.
 */

#include "ove/ove.h"
#include "ove/audio_device.h"
#include "semihosting.h"
#include "qemu_audio_shm.h"
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_OVE_AUDIO

/* ── Ring-buffer helpers (semihosting I/O) ─────────────────────────── */

static int g_sh_fd = -1;
static uint32_t g_out_wpos;
static uint32_t g_in_rpos;

static void ring_write_out(const void *buf, uint32_t len)
{
	int fd = g_sh_fd;
	uint32_t wpos = g_out_wpos;
	uint32_t ring_off = AUDIO_SHM_OUT_RING_OFF + QEMU_RING_OFF_BUF;
	uint32_t mask = AUDIO_SHM_RING_SIZE - 1;
	uint32_t pos_in_ring = wpos & mask;
	uint32_t first = AUDIO_SHM_RING_SIZE - pos_in_ring;

	if (first >= len) {
		sh_seek(fd, ring_off + pos_in_ring);
		sh_write(fd, buf, len);
	} else {
		sh_seek(fd, ring_off + pos_in_ring);
		sh_write(fd, buf, first);
		sh_seek(fd, ring_off);
		sh_write(fd, (const uint8_t *)buf + first, len - first);
	}
	g_out_wpos = wpos + len;
}

static uint32_t ring_read_in(void *buf, uint32_t len)
{
	int fd = g_sh_fd;
	uint32_t rpos = g_in_rpos;
	uint32_t ring_off = AUDIO_SHM_IN_RING_OFF + QEMU_RING_OFF_BUF;
	uint32_t mask = AUDIO_SHM_RING_SIZE - 1;

	uint32_t in_wpos;
	sh_seek(fd, AUDIO_SHM_IN_RING_OFF + QEMU_RING_OFF_WRITE_POS);
	sh_read(fd, &in_wpos, sizeof(in_wpos));

	uint32_t avail = in_wpos - rpos;
	if (avail > AUDIO_SHM_RING_SIZE)
		avail = 0;
	if (len > avail)
		len = avail;
	if (len == 0)
		return 0;

	uint32_t pos_in_ring = rpos & mask;
	uint32_t first = AUDIO_SHM_RING_SIZE - pos_in_ring;

	if (first >= len) {
		sh_seek(fd, ring_off + pos_in_ring);
		sh_read(fd, buf, len);
	} else {
		sh_seek(fd, ring_off + pos_in_ring);
		sh_read(fd, buf, first);
		sh_seek(fd, ring_off);
		sh_read(fd, (uint8_t *)buf + first, len - first);
	}
	g_in_rpos = rpos + len;
	return len;
}

static void flush_positions(void)
{
	int fd = g_sh_fd;
	sh_seek(fd, AUDIO_SHM_OUT_RING_OFF + QEMU_RING_OFF_WRITE_POS);
	sh_write(fd, &g_out_wpos, sizeof(uint32_t));
	sh_seek(fd, AUDIO_SHM_IN_RING_OFF + QEMU_RING_OFF_READ_POS);
	sh_write(fd, &g_in_rpos, sizeof(uint32_t));
}

/* ── QEMU Source Node ───────────────────────────────────────────── */

struct qemu_source_ctx {
	struct ove_audio_fmt fmt;
	struct ove_audio_graph *graph; /* for stats.overruns */
};

static struct qemu_source_ctx g_qemu_source;

static int qemu_source_configure(void *ctx, const struct ove_audio_fmt *in,
				 struct ove_audio_fmt *out)
{
	(void)in;
	struct qemu_source_ctx *sc = (struct qemu_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int qemu_source_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	struct qemu_source_ctx *sc = (struct qemu_source_ctx *)ctx;
	(void)in;
	unsigned int bytes =
		out->frames * out->fmt->channels * ove_audio_sample_size(out->fmt->sample_fmt);

	if (g_sh_fd >= 0) {
		uint32_t got = ring_read_in(out->data, bytes);
		if (got < bytes) {
			memset((uint8_t *)out->data + got, 0, bytes - got);
			if (sc->graph)
				sc->graph->stats.overruns++;
		}
	} else {
		memset(out->data, 0, bytes);
	}
	return OVE_OK;
}

static const struct ove_audio_node_ops qemu_source_ops = {
	.configure = qemu_source_configure,
	.process = qemu_source_process,
};

/* ── QEMU Sink Node ─────────────────────────────────────────────── */

struct qemu_sink_ctx {
	struct ove_audio_fmt fmt;
	struct ove_audio_graph *graph;
	unsigned int frames_per_period;
	ove_thread_t thread;
};

static struct qemu_sink_ctx g_qemu_sink;
#ifdef CONFIG_OVE_ZERO_HEAP
OVE_THREAD_DEFINE(g_audio_thread_storage, 4096);
#endif

static int qemu_sink_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	(void)out;
	struct qemu_sink_ctx *sc = (struct qemu_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt))
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int qemu_sink_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)ctx;
	(void)out;

	if (g_sh_fd >= 0 && in && in->data) {
		unsigned int bytes =
			in->frames * in->fmt->channels * ove_audio_sample_size(in->fmt->sample_fmt);
		ring_write_out(in->data, bytes);
		flush_positions();
	}
	return OVE_OK;
}

static void audio_engine_loop(void *arg)
{
	struct qemu_sink_ctx *sc = (struct qemu_sink_ctx *)arg;
	ove_thread_t self = ove_thread_get_self();

	unsigned int period_ms = (sc->frames_per_period * 1000) / sc->fmt.sample_rate;
	if (period_ms < 1)
		period_ms = 1;

	while (!ove_thread_should_stop(self)) {
		ove_audio_graph_process(sc->graph);
		ove_thread_sleep_ms(period_ms);
	}
}

static int qemu_sink_start(void *ctx)
{
	struct qemu_sink_ctx *sc = (struct qemu_sink_ctx *)ctx;

#ifdef CONFIG_OVE_ZERO_HEAP
	if (ove_thread_init(&sc->thread, &g_audio_thread_storage, "qemu_audio", audio_engine_loop,
			    sc, OVE_PRIO_NORMAL, sizeof(g_audio_thread_storage_stack),
			    g_audio_thread_storage_stack) != OVE_OK) {
#else
	if (ove_thread_create(&sc->thread, "qemu_audio", audio_engine_loop, sc, OVE_PRIO_NORMAL,
			      4096) != OVE_OK) {
#endif
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

static int qemu_sink_stop(void *ctx)
{
	struct qemu_sink_ctx *sc = (struct qemu_sink_ctx *)ctx;
	if (!sc->thread)
		return OVE_OK;

	ove_thread_request_stop(sc->thread);
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_thread_deinit(sc->thread);
#else
	ove_thread_destroy(sc->thread);
#endif
	sc->thread = NULL;
	return OVE_OK;
}

static void qemu_sink_destroy(void *ctx)
{
	struct qemu_sink_ctx *sc = (struct qemu_sink_ctx *)ctx;
	qemu_sink_stop(ctx);
	if (g_sh_fd >= 0) {
		sh_close(g_sh_fd);
		g_sh_fd = -1;
	}
	memset(sc, 0, sizeof(*sc));
}

static const struct ove_audio_node_ops qemu_sink_ops = {
	.configure = qemu_sink_configure,
	.start = qemu_sink_start,
	.stop = qemu_sink_stop,
	.process = qemu_sink_process,
	.destroy = qemu_sink_destroy,
};

/* ── Device Node Factories ──────────────────────────────────────── */

static int qemu_shm_init_once(const struct ove_audio_fmt *fmt, unsigned int frames_per_buffer)
{
	if (g_sh_fd >= 0)
		return OVE_OK; /* already open */

	g_sh_fd = sh_open(AUDIO_SHM_PATH, 7); /* "r+b" */

	if (g_sh_fd >= 0) {
		/* Write output ring header. */
		uint32_t zero = 0;
		uint32_t sr = fmt->sample_rate;
		uint16_t ch = fmt->channels;
		uint16_t bd = ove_audio_sample_size(fmt->sample_fmt) * 8;
		uint32_t sz = AUDIO_SHM_RING_SIZE;

		sh_seek(g_sh_fd, AUDIO_SHM_OUT_RING_OFF);
		sh_write(g_sh_fd, &zero, 4); /* write_pos */
		sh_write(g_sh_fd, &zero, 4); /* read_pos */
		sh_write(g_sh_fd, &sr, 4);   /* sample_rate */
		sh_write(g_sh_fd, &ch, 2);   /* channels */
		sh_write(g_sh_fd, &bd, 2);   /* bit_depth */
		sh_write(g_sh_fd, &sz, 4);   /* size */
		sh_write(g_sh_fd, &zero, 4); /* underruns */
		sh_write(g_sh_fd, &zero, 4); /* overruns */
		sh_write(g_sh_fd, &zero, 4); /* _reserved */

		/* Write input ring header (same format). */
		sh_seek(g_sh_fd, AUDIO_SHM_IN_RING_OFF);
		sh_write(g_sh_fd, &zero, 4);
		sh_write(g_sh_fd, &zero, 4);
		sh_write(g_sh_fd, &sr, 4);
		sh_write(g_sh_fd, &ch, 2);
		sh_write(g_sh_fd, &bd, 2);
		sh_write(g_sh_fd, &sz, 4);
		sh_write(g_sh_fd, &zero, 4);
		sh_write(g_sh_fd, &zero, 4);
		sh_write(g_sh_fd, &zero, 4);
	}
	/* Not an error if headless — we fall back to silent processing */

	g_out_wpos = 0;
	g_in_rpos = 0;
	return OVE_OK;
}

int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	qemu_shm_init_once(&cfg->fmt, g->frames_per_period);

	struct qemu_source_ctx *ctx = &g_qemu_source;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->graph = g;

	return ove_audio_graph_add_node(g, &qemu_source_ops, ctx, name, OVE_AUDIO_NODE_SOURCE);
}

int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	qemu_shm_init_once(&cfg->fmt, g->frames_per_period);

	struct qemu_sink_ctx *ctx = &g_qemu_sink;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->graph = g;
	ctx->frames_per_period = g->frames_per_period;

	return ove_audio_graph_add_node(g, &qemu_sink_ops, ctx, name, OVE_AUDIO_NODE_SINK);
}

/* Stub for sim_board.c which calls this during init.  On QEMU the audio
 * plugin is not used — audio goes directly through semihosting SHM. */
int ove_sim_audio_register(uint32_t sample_rate, uint16_t channels, uint16_t bit_depth,
			   uint32_t buffer_frames)
{
	(void)sample_rate;
	(void)channels;
	(void)bit_depth;
	(void)buffer_frames;
	return 0;
}

#endif /* CONFIG_OVE_AUDIO */
