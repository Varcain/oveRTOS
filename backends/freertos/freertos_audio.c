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
#include "FreeRTOS.h"
#include "task.h"

#ifdef CONFIG_OVE_I2S
#include "ove/i2s.h"
#endif

#include <string.h>
#include <stdlib.h>

#ifdef CONFIG_OVE_AUDIO

/* ========================================================================= */
/* INTERNAL STATE                                                            */
/* ========================================================================= */

typedef enum {
	BUFFER_FIRST_HALF = 0,
	BUFFER_SECOND_HALF = 1
} buffer_phase_t;

#define DEFAULT_AUDIO_PRIORITY  (tskIDLE_PRIORITY + 7)
#define DEFAULT_AUDIO_STACK     (configMINIMAL_STACK_SIZE * 32)

/*
 * Board-provided codec initialisation (weak default does nothing).
 * The board overrides this to configure the audio codec (e.g. WM8994)
 * via I2C register writes after the I2S bus is configured.
 */
__attribute__((weak))
void ove_board_audio_codec_init(uint32_t sample_rate, int input_device)
{
	(void)sample_rate;
	(void)input_device;
}

/* ========================================================================= */
/* I2S SHARED STATE (forward declarations for source/sink)                   */
/* ========================================================================= */

#ifdef CONFIG_OVE_I2S

struct i2s_sink_ctx {
	struct ove_audio_fmt    fmt;
	struct ove_audio_graph *graph;
	unsigned int            frames_per_period;
	unsigned int            thread_priority;
	unsigned int            thread_stack_size;
	TaskHandle_t            task_handle;
	ove_i2s_t               i2s;
	volatile buffer_phase_t current_rx_phase;
	volatile buffer_phase_t current_tx_phase;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_i2s_storage_t       i2s_storage;
	StaticTask_t            task_tcb;
	StackType_t             task_stack[DEFAULT_AUDIO_STACK];
#endif
};

/* Single static sink context — only one I2S graph can be active */
static struct i2s_sink_ctx g_sink_ctx;

/* ========================================================================= */
/* I2S SOURCE NODE                                                           */
/* ========================================================================= */

struct i2s_source_ctx {
	struct ove_audio_fmt fmt;
	unsigned int         input_device;
};

static int i2s_source_configure(void *ctx, const struct ove_audio_fmt *in,
				struct ove_audio_fmt *out)
{
	(void)in;
	struct i2s_source_ctx *sc = (struct i2s_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int i2s_source_process(void *ctx, const struct ove_audio_buf *in,
			      struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	struct i2s_sink_ctx *sink = &g_sink_ctx;  /* forward ref */
	int16_t *rx_ptr = (int16_t *)ove_i2s_rx_buf(sink->i2s);
	if (rx_ptr == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	unsigned int bytes = out->frames * out->fmt->channels *
			     ove_audio_sample_size(out->fmt->sample_fmt);
	memcpy(out->data, rx_ptr, bytes);
	return OVE_OK;
}

static const struct ove_audio_node_ops i2s_source_ops = {
	.configure = i2s_source_configure,
	.process   = i2s_source_process,
};

/* ========================================================================= */
/* I2S SINK NODE                                                             */
/* ========================================================================= */

static int i2s_sink_configure(void *ctx, const struct ove_audio_fmt *in,
			      struct ove_audio_fmt *out)
{
	(void)out;
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt))
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int i2s_sink_process(void *ctx, const struct ove_audio_buf *in,
			    struct ove_audio_buf *out)
{
	(void)out;
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;
	int16_t *tx_ptr = (int16_t *)ove_i2s_tx_buf(sc->i2s);
	if (tx_ptr == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	unsigned int bytes = in->frames * in->fmt->channels *
			     ove_audio_sample_size(in->fmt->sample_fmt);
	memcpy(tx_ptr, in->data, bytes);
	return OVE_OK;
}

/* ── ISR callbacks ──────────────────────────────────────────────── */

static void audio_rx_complete_callback(ove_i2s_t i2s, void *user_data)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)user_data;
	if (sc->task_handle == NULL)
		return;

	sc->current_rx_phase = (buffer_phase_t)i2s->rx_completed_half;

	BaseType_t yield_required = pdFALSE;
	vTaskNotifyGiveFromISR(sc->task_handle, &yield_required);
	portYIELD_FROM_ISR(yield_required);
}

static void audio_tx_complete_callback(ove_i2s_t i2s, void *user_data)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)user_data;
	if (sc->task_handle == NULL)
		return;

	sc->current_tx_phase = (buffer_phase_t)i2s->tx_completed_half;
	/* Only RX callback drives the audio task — see comment in old code */
}

/* ── Engine task ────────────────────────────────────────────────── */

static void audio_engine_task(void *pvParameters)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)pvParameters;

	buffer_phase_t last_rx_phase = BUFFER_SECOND_HALF;
	buffer_phase_t last_tx_phase = BUFFER_SECOND_HALF;
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		buffer_phase_t process_rx_phase = sc->current_rx_phase;
		buffer_phase_t process_tx_phase = sc->current_tx_phase;

		int skip = 0;
		if (process_tx_phase == last_tx_phase)
			skip = 1;
		last_tx_phase = process_tx_phase;

		if (process_rx_phase == last_rx_phase)
			skip = 1;
		last_rx_phase = process_rx_phase;

		if (skip) {
			sc->graph->stats.underruns++;
			continue;
		}

		ove_audio_graph_process(sc->graph);
	}
}

/* ── Start / Stop ───────────────────────────────────────────────── */

static int i2s_sink_start(void *ctx)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;

#ifdef CONFIG_OVE_ZERO_HEAP
	sc->task_handle = xTaskCreateStatic(
		audio_engine_task, "Audio", DEFAULT_AUDIO_STACK,
		sc, sc->thread_priority, sc->task_stack, &sc->task_tcb);
	if (sc->task_handle == NULL)
		return OVE_ERR_NO_MEMORY;
#else
	BaseType_t ret = xTaskCreate(audio_engine_task, "Audio",
				     sc->thread_stack_size, sc,
				     sc->thread_priority,
				     &sc->task_handle);
	if (ret != pdPASS)
		return OVE_ERR_NO_MEMORY;
#endif

	return ove_i2s_start(sc->i2s);
}

static int i2s_sink_stop(void *ctx)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;

	ove_i2s_pause(sc->i2s);

	if (sc->task_handle != NULL) {
		vTaskDelete(sc->task_handle);
		sc->task_handle = NULL;
	}
	return OVE_OK;
}

static const struct ove_audio_node_ops i2s_sink_ops = {
	.configure = i2s_sink_configure,
	.start     = i2s_sink_start,
	.stop      = i2s_sink_stop,
	.process   = i2s_sink_process,
};

#endif /* CONFIG_OVE_I2S */

/* ========================================================================= */
/* DEVICE NODE FACTORIES                                                     */
/* ========================================================================= */

int ove_audio_device_source(struct ove_audio_graph *g,
			    const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

#ifdef CONFIG_OVE_I2S
	if (cfg->transport == OVE_AUDIO_TRANSPORT_I2S) {
		struct i2s_source_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
		if (!ctx)
			return OVE_ERR_NO_MEMORY;

		memset(ctx, 0, sizeof(*ctx));
		ctx->fmt = cfg->fmt;
		ctx->input_device = cfg->i2s.input_device;

		int idx = ove_audio_graph_add_node(g, &i2s_source_ops, ctx,
						   name, OVE_AUDIO_NODE_SOURCE);
		if (idx < 0)
			OVE_BACKEND_FREE(ctx);
		return idx;
	}
#endif
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_audio_device_sink(struct ove_audio_graph *g,
			  const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

#ifdef CONFIG_OVE_I2S
	if (cfg->transport == OVE_AUDIO_TRANSPORT_I2S) {
		struct i2s_sink_ctx *ctx = &g_sink_ctx;
		memset(ctx, 0, sizeof(*ctx));
		ctx->fmt = cfg->fmt;
		ctx->graph = g;
		ctx->frames_per_period = g->frames_per_period;
		ctx->thread_priority = cfg->thread_priority
			? cfg->thread_priority : DEFAULT_AUDIO_PRIORITY;
		ctx->thread_stack_size = cfg->thread_stack_size
			? cfg->thread_stack_size : DEFAULT_AUDIO_STACK;

		/* Create I2S bus */
		struct ove_i2s_cfg i2s_cfg = {
			.instance        = 1,  /* SAI2 */
			.sample_rate     = cfg->fmt.sample_rate,
			.bit_depth       = 16,
			.channels        = cfg->fmt.channels,
			.direction       = OVE_I2S_DIR_TXRX,
			.dma_buf_samples = g->frames_per_period * cfg->fmt.channels * 2,
		};

		/* DMA buffers MUST be in non-cacheable memory (DTCM on Cortex-M7).
		 * Heap is in cached SRAM — cannot use ove_i2s_create() here.
		 * Board linker script places .RxBUF / .TxBUF in DTCM. */
		static uint8_t tx_dma[4096] __attribute__((section(".TxBUF"), aligned(32)));
		static uint8_t rx_dma[4096] __attribute__((section(".RxBUF"), aligned(32)));
#ifndef CONFIG_OVE_ZERO_HEAP
		static ove_i2s_storage_t i2s_stor;
		int ret = ove_i2s_init(&ctx->i2s, &i2s_stor,
				       tx_dma, rx_dma, &i2s_cfg);
#else
		int ret = ove_i2s_init(&ctx->i2s, &ctx->i2s_storage,
				       tx_dma, rx_dma, &i2s_cfg);
#endif
		if (ret != OVE_OK)
			return ret;

		/* Board-specific codec init (WM8994 etc.) */
		ove_board_audio_codec_init(cfg->fmt.sample_rate,
					   cfg->i2s.input_device);

		/* Register ISR callbacks with sink context as user_data */
		ove_i2s_set_tx_callback(ctx->i2s, audio_tx_complete_callback, ctx);
		ove_i2s_set_rx_callback(ctx->i2s, audio_rx_complete_callback, ctx);

		int idx = ove_audio_graph_add_node(g, &i2s_sink_ops, ctx,
						   name, OVE_AUDIO_NODE_SINK);
		return idx;
	}
#endif
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_AUDIO */
