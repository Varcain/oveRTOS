/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/audio.h"
#include "ove_backend_common.h"
#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "app_conf.h"

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
#define BLOCK_SIZE         (BYTES_PER_SAMPLE * DSP_BUFFER_SIZE)
#define SLAB_BLOCK_SIZE    ((BLOCK_SIZE + 31) & ~31)
#define DEFAULT_INITIAL_BLOCKS  4
#define DEFAULT_BLOCK_COUNT     (DEFAULT_INITIAL_BLOCKS + 4)
#define DEFAULT_AUDIO_PRIORITY  2
#define DEFAULT_AUDIO_STACK     4096
#define I2S_TIMEOUT             1000

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(audio_slab, __dtcm_noinit_section,
				 SLAB_BLOCK_SIZE, DEFAULT_BLOCK_COUNT, 32);

static ove_audio_process_fn g_process_fn;
static void *g_user_data;
static unsigned int g_frames_per_buffer;
static unsigned int g_initial_blocks;
static unsigned int g_thread_priority;

static const struct device *dev_rx;
static const struct device *dev_tx;

static K_SEM_DEFINE(i2s_ready_sem, 0, 1);

/* Audio thread (backend-owned) */
static void audio_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_sem_take(&i2s_ready_sem, K_FOREVER);
	printk("Audio thread running\n");

	for (;;) {
		void *rx_block;
		uint32_t block_size;
		int ret;

		ret = i2s_read(dev_rx, &rx_block, &block_size);
		if (ret < 0) {
			if (ret == -EIO) {
				k_sleep(K_MSEC(50));
				continue;
			}
			continue;
		}

		void *tx_block;
		ret = k_mem_slab_alloc(&audio_slab, &tx_block, K_NO_WAIT);
		if (ret < 0) {
			k_mem_slab_free(&audio_slab, rx_block);
			continue;
		}

		if (g_process_fn != NULL) {
			g_process_fn((int16_t *)tx_block,
				     (const int16_t *)rx_block,
				     g_frames_per_buffer, g_user_data);
		} else {
			memcpy(tx_block, rx_block, BLOCK_SIZE);
		}

		k_mem_slab_free(&audio_slab, rx_block);

		ret = i2s_write(dev_tx, tx_block, BLOCK_SIZE);
		if (ret < 0) {
			k_mem_slab_free(&audio_slab, tx_block);
		}
	}
}

K_THREAD_DEFINE(zephyr_audio_thread, DEFAULT_AUDIO_STACK,
		audio_thread_fn, NULL, NULL, NULL,
		DEFAULT_AUDIO_PRIORITY, 0, 0);

/* I2S helpers */
static bool configure_i2s_streams(void)
{
	struct i2s_config config;
	int ret;

	config.word_size = 16;
	config.channels = 1;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
	config.options = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER;
	config.frame_clk_freq = DSP_RATE;
	config.mem_slab = &audio_slab;
	config.block_size = BLOCK_SIZE;
	config.timeout = I2S_TIMEOUT;

	if (dev_rx == dev_tx) {
		ret = i2s_configure(dev_rx, I2S_DIR_BOTH, &config);
		if (ret == 0) {
			return true;
		}
		if (ret != -ENOSYS) {
			return false;
		}
	}

	struct i2s_config rx_config = config;
	rx_config.options = I2S_OPT_BIT_CLK_SLAVE | I2S_OPT_FRAME_CLK_SLAVE;

	ret = i2s_configure(dev_rx, I2S_DIR_RX, &rx_config);
	if (ret < 0) {
		return false;
	}

	ret = i2s_configure(dev_tx, I2S_DIR_TX, &config);
	if (ret < 0) {
		return false;
	}

	return true;
}

static bool start_i2s_streams(void)
{
	int ret;

	for (unsigned int i = 0; i < g_initial_blocks; i++) {
		void *mem_block;
		ret = k_mem_slab_alloc(&audio_slab, &mem_block, K_NO_WAIT);
		if (ret < 0) {
			return false;
		}
		memset(mem_block, 0, BLOCK_SIZE);
		ret = i2s_write(dev_tx, mem_block, BLOCK_SIZE);
		if (ret < 0) {
			return false;
		}
	}

	if (dev_rx == dev_tx) {
		ret = i2s_trigger(dev_rx, I2S_DIR_BOTH, I2S_TRIGGER_START);
		if (ret == 0) {
			return true;
		}
		if (ret != -ENOSYS) {
			return false;
		}
	}

	ret = i2s_trigger(dev_tx, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		return false;
	}
	ret = i2s_trigger(dev_rx, I2S_DIR_RX, I2S_TRIGGER_START);
	if (ret < 0) {
		return false;
	}

	return true;
}

/* Ops implementation */

int ove_audio_init(const struct ove_audio_config *cfg,
			     ove_audio_process_fn fn, void *user_data)
{
	if (cfg == NULL || fn == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	g_process_fn = fn;
	g_user_data = user_data;
	g_frames_per_buffer = cfg->frames_per_buffer;
	g_initial_blocks = cfg->num_buffers ? cfg->num_buffers
					    : DEFAULT_INITIAL_BLOCKS;
	g_thread_priority = cfg->thread_priority ? cfg->thread_priority
						 : DEFAULT_AUDIO_PRIORITY;

	/* Apply runtime thread priority override */
	k_thread_priority_set(zephyr_audio_thread, g_thread_priority);

	dev_rx = DEVICE_DT_GET(I2S_RX_NODE);
	dev_tx = DEVICE_DT_GET(I2S_TX_NODE);

	if (!device_is_ready(dev_rx)) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (dev_rx != dev_tx && !device_is_ready(dev_tx)) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	if (!configure_i2s_streams()) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	/* Enable SAI2_A MCLK output AFTER I2S configure (which sets up SAI
	 * registers) but BEFORE codec configure (WM8994 needs MCLK present
	 * for I2C register writes to work correctly).
	 * SAI2 Block A CR1 register, bit 16 = MCKEN. */
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
	audio_cfg.dai_cfg.i2s.frame_clk_freq = DSP_RATE;
	audio_cfg.dai_cfg.i2s.mem_slab = &audio_slab;
	audio_cfg.dai_cfg.i2s.block_size = BLOCK_SIZE;
	audio_codec_configure(codec_dev, &audio_cfg);

	audio_codec_start(codec_dev, AUDIO_DAI_DIR_TX);
	audio_codec_start(codec_dev, AUDIO_DAI_DIR_RX);
	k_msleep(1000);
#endif

	return OVE_OK;
}

int ove_audio_start(void)
{
	if (!start_i2s_streams()) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	k_sem_give(&i2s_ready_sem);
	return OVE_OK;
}

int ove_audio_stop(void)
{
	if (dev_rx == dev_tx) {
		i2s_trigger(dev_rx, I2S_DIR_BOTH, I2S_TRIGGER_STOP);
	} else {
		i2s_trigger(dev_rx, I2S_DIR_RX, I2S_TRIGGER_STOP);
		i2s_trigger(dev_tx, I2S_DIR_TX, I2S_TRIGGER_STOP);
	}
	return OVE_OK;
}
#else /* !HAVE_I2S_NODES */

int ove_audio_init(const struct ove_audio_config *cfg,
			     ove_audio_process_fn fn, void *user_data)
{
	(void)cfg; (void)fn; (void)user_data;
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_audio_start(void) { return OVE_ERR_NOT_SUPPORTED; }
int ove_audio_stop(void) { return OVE_OK; }

#endif /* HAVE_I2S_NODES */
