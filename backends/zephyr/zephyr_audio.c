/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/types.h"
#include "ove/audio_device.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "board_desc.h"

#ifdef CONFIG_OVE_AUDIO

/* I2S device tree nodes */
#if DT_NODE_EXISTS(DT_NODELABEL(i2s_rxtx))
#define I2S_RX_NODE DT_NODELABEL(i2s_rxtx)
#define I2S_TX_NODE I2S_RX_NODE
#define HAVE_I2S_NODES 1
#elif DT_NODE_EXISTS(DT_NODELABEL(i2s_rx)) && \
      DT_NODE_EXISTS(DT_NODELABEL(i2s_tx))
#define I2S_RX_NODE DT_NODELABEL(i2s_rx)
#define I2S_TX_NODE DT_NODELABEL(i2s_tx)
#define HAVE_I2S_NODES 1
#else
#define HAVE_I2S_NODES 0
#endif

#if HAVE_I2S_NODES

#define BYTES_PER_SAMPLE   sizeof(int16_t)
#define BLOCK_SIZE         (BYTES_PER_SAMPLE * OVE_AUDIO_I2S_BUFFER_SAMPLES)
#define SLAB_BLOCK_SIZE    ((BLOCK_SIZE + 31) & ~31)
#define DEFAULT_INITIAL_BLOCKS  4
#define DEFAULT_BLOCK_COUNT     (DEFAULT_INITIAL_BLOCKS + 4)
#define DEFAULT_AUDIO_PRIORITY  2
#define DEFAULT_AUDIO_STACK     4096
#define I2S_TIMEOUT             1000

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_slab, __dtcm_noinit_section,
				 SLAB_BLOCK_SIZE, DEFAULT_BLOCK_COUNT, 32);

static const struct device *dev_rx;
static const struct device *dev_tx;

static K_SEM_DEFINE(i2s_ready_sem, 0, 1);

/* ── I2S Source Node ────────────────────────────────────────────── */

struct zephyr_i2s_source_ctx {
	struct ove_audio_fmt fmt;
	void                *current_rx_block;
	uint32_t             current_block_size;
};

static int zephyr_source_configure(void *ctx, const struct ove_audio_fmt *in,
				   struct ove_audio_fmt *out)
{
	(void)in;
	struct zephyr_i2s_source_ctx *sc = (struct zephyr_i2s_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int zephyr_source_process(void *ctx, const struct ove_audio_buf *in,
				 struct ove_audio_buf *out)
{
	(void)in;
	struct zephyr_i2s_source_ctx *sc = (struct zephyr_i2s_source_ctx *)ctx;

	/* Read from I2S RX */
	int ret = i2s_read(dev_rx, &sc->current_rx_block,
			   &sc->current_block_size);
	if (ret < 0) {
		memset(out->data, 0,
		       out->frames * out->fmt->channels *
		       ove_audio_sample_size(out->fmt->sample_fmt));
		return (ret == -EIO) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
	}

	unsigned int bytes = out->frames * out->fmt->channels *
			     ove_audio_sample_size(out->fmt->sample_fmt);
	memcpy(out->data, sc->current_rx_block, bytes);
	k_mem_slab_free(&audio_slab, sc->current_rx_block);
	sc->current_rx_block = NULL;

	return OVE_OK;
}

static const struct ove_audio_node_ops zephyr_source_ops = {
	.configure = zephyr_source_configure,
	.process   = zephyr_source_process,
};

/* ── I2S Sink Node ──────────────────────────────────────────────── */

struct zephyr_i2s_sink_ctx {
	struct ove_audio_fmt    fmt;
	struct ove_audio_graph *graph;
	unsigned int            frames_per_period;
	unsigned int            initial_blocks;
	unsigned int            thread_priority;
};

/* Forward declaration for thread function */
static void zephyr_audio_thread_fn(void *p1, void *p2, void *p3);

K_THREAD_DEFINE(zephyr_audio_thread, DEFAULT_AUDIO_STACK,
		zephyr_audio_thread_fn, NULL, NULL, NULL,
		DEFAULT_AUDIO_PRIORITY, 0, 0);

static struct zephyr_i2s_sink_ctx g_zephyr_sink_ctx;

static void zephyr_audio_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&i2s_ready_sem, K_FOREVER);
	printk("Audio graph thread running\n");

	for (;;) {
		/* Walk the entire graph: source reads I2S RX,
		 * processors transform, sink writes I2S TX */
		int ret = ove_audio_graph_process(g_zephyr_sink_ctx.graph);
		if (ret != OVE_OK)
			k_sleep(K_MSEC(50));
	}
}

static int zephyr_sink_configure(void *ctx, const struct ove_audio_fmt *in,
				 struct ove_audio_fmt *out)
{
	(void)out;
	struct zephyr_i2s_sink_ctx *sc = (struct zephyr_i2s_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt))
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int zephyr_sink_process(void *ctx, const struct ove_audio_buf *in,
			       struct ove_audio_buf *out)
{
	(void)ctx;
	(void)out;

	/* Allocate a TX slab block and write to I2S */
	void *tx_block;
	int ret = k_mem_slab_alloc(&audio_slab, &tx_block, K_NO_WAIT);
	if (ret < 0)
		return OVE_ERR_NO_MEMORY;

	unsigned int bytes = in->frames * in->fmt->channels *
			     ove_audio_sample_size(in->fmt->sample_fmt);
	memcpy(tx_block, in->data, bytes);

	ret = i2s_write(dev_tx, tx_block, bytes);
	if (ret < 0) {
		k_mem_slab_free(&audio_slab, tx_block);
		return OVE_ERR_NOT_SUPPORTED;
	}

	return OVE_OK;
}

/* ── I2S helpers ────────────────────────────────────────────────── */

static bool configure_i2s_streams(void)
{
	struct i2s_config config;
	int ret;

	config.word_size = 16;
	config.channels = 1;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
	config.frame_clk_freq = OVE_AUDIO_I2S_SAMPLE_RATE;
	config.mem_slab = &audio_slab;
	config.block_size = BLOCK_SIZE;
	config.timeout = I2S_TIMEOUT;

	if (dev_rx == dev_tx) {
		ret = i2s_configure(dev_rx, I2S_DIR_BOTH, &config);
		if (ret == 0)
			return true;
		if (ret != -ENOSYS)
			return false;
	}

	struct i2s_config rx_config = config;
	rx_config.options = I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE;

	ret = i2s_configure(dev_rx, I2S_DIR_RX, &rx_config);
	if (ret < 0)
		return false;

	ret = i2s_configure(dev_tx, I2S_DIR_TX, &config);
	if (ret < 0)
		return false;

	return true;
}

static bool start_i2s_streams(unsigned int initial_blocks)
{
	int ret;

	for (unsigned int i = 0; i < initial_blocks; i++) {
		void *mem_block;
		ret = k_mem_slab_alloc(&audio_slab, &mem_block, K_NO_WAIT);
		if (ret < 0)
			return false;
		memset(mem_block, 0, BLOCK_SIZE);
		ret = i2s_write(dev_tx, mem_block, BLOCK_SIZE);
		if (ret < 0)
			return false;
	}

	if (dev_rx == dev_tx) {
		ret = i2s_trigger(dev_rx, I2S_DIR_BOTH, I2S_TRIGGER_START);
		if (ret == 0)
			return true;
		if (ret != -ENOSYS)
			return false;
	}

	ret = i2s_trigger(dev_tx, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0)
		return false;
	ret = i2s_trigger(dev_rx, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0)
		return false;

	return true;
}

static int zephyr_sink_start(void *ctx)
{
	struct zephyr_i2s_sink_ctx *sc = (struct zephyr_i2s_sink_ctx *)ctx;

	if (!start_i2s_streams(sc->initial_blocks))
		return OVE_ERR_NOT_SUPPORTED;

	k_sem_give(&i2s_ready_sem);
	return OVE_OK;
}

static int zephyr_sink_stop(void *ctx)
{
	(void)ctx;
	if (dev_rx == dev_tx) {
		i2s_trigger(dev_rx, I2S_DIR_BOTH, I2S_TRIGGER_STOP);
	} else {
		i2s_trigger(dev_rx, I2S_DIR_RX, I2S_TRIGGER_STOP);
		i2s_trigger(dev_tx, I2S_DIR_TX, I2S_TRIGGER_STOP);
	}
	return OVE_OK;
}

static const struct ove_audio_node_ops zephyr_sink_ops = {
	.configure = zephyr_sink_configure,
	.start     = zephyr_sink_start,
	.stop      = zephyr_sink_stop,
	.process   = zephyr_sink_process,
};

/* ========================================================================= */
/* DEVICE NODE FACTORIES                                                     */
/* ========================================================================= */

static int zephyr_i2s_init_once(void)
{
	static int initialized;
	if (initialized)
		return OVE_OK;

	dev_rx = DEVICE_DT_GET(I2S_RX_NODE);
	dev_tx = DEVICE_DT_GET(I2S_TX_NODE);

	if (!device_is_ready(dev_rx))
		return OVE_ERR_NOT_SUPPORTED;
	if (dev_rx != dev_tx && !device_is_ready(dev_tx))
		return OVE_ERR_NOT_SUPPORTED;

	if (!configure_i2s_streams())
		return OVE_ERR_NOT_SUPPORTED;

	/* Enable SAI2_A MCLK output AFTER I2S configure but BEFORE codec. */
	{
		volatile uint32_t *sai2a_cr1 = (volatile uint32_t *)0x40015C04U;
		*sai2a_cr1 |= (1U << 16);
	}

	/* Configure codec */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(audio_codec), okay)
	const struct device *const codec_dev =
		DEVICE_DT_GET(DT_NODELABEL(audio_codec));
	struct audio_codec_cfg audio_cfg;

	audio_cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	audio_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	audio_cfg.dai_cfg.i2s.word_size = 16;
	audio_cfg.dai_cfg.i2s.channels = 1;
	audio_cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
	audio_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_MASTER;
	audio_cfg.dai_cfg.i2s.frame_clk_freq = OVE_AUDIO_I2S_SAMPLE_RATE;
	audio_cfg.dai_cfg.i2s.mem_slab = &audio_slab;
	audio_cfg.dai_cfg.i2s.block_size = BLOCK_SIZE;
	audio_codec_configure(codec_dev, &audio_cfg);

	audio_codec_start(codec_dev, AUDIO_DAI_DIR_TX);
	audio_codec_start(codec_dev, AUDIO_DAI_DIR_RX);
	k_msleep(1000);
#endif

	initialized = 1;
	return OVE_OK;
}

int ove_audio_device_source(struct ove_audio_graph *g,
			    const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	int ret = zephyr_i2s_init_once();
	if (ret != OVE_OK)
		return ret;

	static struct zephyr_i2s_source_ctx source_ctx;
	memset(&source_ctx, 0, sizeof(source_ctx));
	source_ctx.fmt = cfg->fmt;

	k_thread_priority_set(zephyr_audio_thread,
			      cfg->thread_priority ? cfg->thread_priority
						   : DEFAULT_AUDIO_PRIORITY);

	return ove_audio_graph_add_node(g, &zephyr_source_ops, &source_ctx,
					name, OVE_AUDIO_NODE_SOURCE);
}

int ove_audio_device_sink(struct ove_audio_graph *g,
			  const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;
	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	int ret = zephyr_i2s_init_once();
	if (ret != OVE_OK)
		return ret;

	struct zephyr_i2s_sink_ctx *ctx = &g_zephyr_sink_ctx;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->graph = g;
	ctx->frames_per_period = g->frames_per_period;
	ctx->initial_blocks = cfg->num_buffers ? cfg->num_buffers
					       : DEFAULT_INITIAL_BLOCKS;
	ctx->thread_priority = cfg->thread_priority ? cfg->thread_priority
						    : DEFAULT_AUDIO_PRIORITY;

	return ove_audio_graph_add_node(g, &zephyr_sink_ops, ctx, name,
					OVE_AUDIO_NODE_SINK);
}

#else /* !HAVE_I2S_NODES */

int ove_audio_device_source(struct ove_audio_graph *g,
			    const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	(void)g; (void)cfg; (void)name;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_audio_device_sink(struct ove_audio_graph *g,
			  const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	(void)g; (void)cfg; (void)name;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* HAVE_I2S_NODES */
#endif /* CONFIG_OVE_AUDIO */
