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

typedef enum { BUFFER_FIRST_HALF = 0, BUFFER_SECOND_HALF = 1 } buffer_phase_t;

#define DEFAULT_AUDIO_PRIORITY (tskIDLE_PRIORITY + 7)
#define DEFAULT_AUDIO_STACK (configMINIMAL_STACK_SIZE * 32)

/*
 * Board-provided codec initialisation (weak default does nothing).
 * The board overrides this to configure the audio codec (e.g. WM8994)
 * via I2C register writes after the I2S bus is configured.
 */
__attribute__((weak)) void ove_board_audio_codec_init(uint32_t sample_rate, int input_device)
{
	(void)sample_rate;
	(void)input_device;
}

/* ========================================================================= */
/* I2S SHARED STATE (forward declarations for source/sink)                   */
/* ========================================================================= */

#ifdef CONFIG_OVE_I2S

struct i2s_sink_ctx {
	struct ove_audio_fmt fmt;
	struct ove_audio_graph *graph;
	unsigned int frames_per_period;
	unsigned int thread_priority;
	unsigned int thread_stack_size;
	TaskHandle_t task_handle;
	ove_i2s_t i2s;
	unsigned int tx_slots[2];   /* slot indices for L, R */
	unsigned int tx_slot_count; /* total slots per DMA frame */
	volatile buffer_phase_t current_rx_phase;
	volatile buffer_phase_t current_tx_phase;
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_i2s_storage_t i2s_storage;
	StaticTask_t task_tcb;
	StackType_t task_stack[DEFAULT_AUDIO_STACK];
#endif
};

/* Single static sink context — only one I2S graph can be active */
static struct i2s_sink_ctx g_sink_ctx;

/* ========================================================================= */
/* I2S SOURCE NODE                                                           */
/* ========================================================================= */

struct i2s_source_ctx {
	struct ove_audio_fmt fmt;
	unsigned int input_device;
	unsigned int rx_slots[2]; /* slot indices for L, R */
	unsigned int slot_count;  /* total slots per DMA frame */
};

/* Single static source context — matches the single static sink.  Keeping it
 * static avoids an OVE_BACKEND_MALLOC() call that fails in zero-heap builds. */
static struct i2s_source_ctx g_source_ctx;

static int i2s_source_configure(void *ctx, const struct ove_audio_fmt *in,
				struct ove_audio_fmt *out)
{
	(void)in;
	struct i2s_source_ctx *sc = (struct i2s_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int i2s_source_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)in;
	struct i2s_source_ctx *sc = (struct i2s_source_ctx *)ctx;
	struct i2s_sink_ctx *sink = &g_sink_ctx; /* forward ref */
	/* Source relies on the sink's I2S instance for shared DMA. If the
	 * sink hasn't been started yet, the global i2s handle is NULL. */
	if (sink->i2s == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	int16_t *rx_ptr = (int16_t *)ove_i2s_rx_buf(sink->i2s);
	if (rx_ptr == NULL)
		return OVE_ERR_NOT_SUPPORTED;

	/* Extract configured channels from the I2S DMA slot layout.
	 * DMA delivers [slot0, slot1, ..., slotN-1] per frame.
	 * We extract the slots specified in rx_slots[] to produce
	 * clean mono/stereo PCM for the graph. */
	int16_t *dst = (int16_t *)out->data;
	unsigned int ch = out->fmt->channels;
	unsigned int nslots = sc->slot_count;
	unsigned int frames = out->frames;

	for (unsigned int f = 0; f < frames; f++) {
		for (unsigned int c = 0; c < ch && c < 2; c++)
			dst[f * ch + c] = rx_ptr[f * nslots + sc->rx_slots[c]];
	}
	return OVE_OK;
}

static const struct ove_audio_node_ops i2s_source_ops = {
	.configure = i2s_source_configure,
	.process = i2s_source_process,
};

/* ========================================================================= */
/* I2S SINK NODE                                                             */
/* ========================================================================= */

static int i2s_sink_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	(void)out;
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt))
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int i2s_sink_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)out;
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;
	int16_t *tx_ptr = (int16_t *)ove_i2s_tx_buf(sc->i2s);
	if (tx_ptr == NULL)
		return OVE_ERR_NOT_SUPPORTED;

	/* Place clean PCM into the correct TX DMA slots.
	 * Zero all other slots to avoid stale data on the bus. */
	const int16_t *src = (const int16_t *)in->data;
	unsigned int ch = in->fmt->channels;
	unsigned int frames = in->frames;
	unsigned int nslots = sc->tx_slot_count;

	memset(tx_ptr, 0, frames * nslots * sizeof(int16_t));
	for (unsigned int f = 0; f < frames; f++) {
		for (unsigned int c = 0; c < ch && c < 2; c++)
			tx_ptr[f * nslots + sc->tx_slots[c]] = src[f * ch + c];
	}
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
	sc->task_handle = xTaskCreateStatic(audio_engine_task, "Audio", DEFAULT_AUDIO_STACK, sc,
					    sc->thread_priority, sc->task_stack, &sc->task_tcb);
	if (sc->task_handle == NULL)
		return OVE_ERR_NO_MEMORY;
#else
	BaseType_t ret = xTaskCreate(audio_engine_task, "Audio", sc->thread_stack_size, sc,
				     sc->thread_priority, &sc->task_handle);
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
	.start = i2s_sink_start,
	.stop = i2s_sink_stop,
	.process = i2s_sink_process,
};

#endif /* CONFIG_OVE_I2S */

/* ========================================================================= */
/* DEVICE NODE FACTORIES                                                     */
/* ========================================================================= */

int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

#ifdef CONFIG_OVE_I2S
	if (cfg->transport == OVE_AUDIO_TRANSPORT_I2S) {
		struct i2s_source_ctx *ctx = &g_source_ctx;
		memset(ctx, 0, sizeof(*ctx));
		ctx->fmt = cfg->fmt;
		ctx->input_device = cfg->i2s.input_device;
		/* Default I2S slot mapping for STM32F746-DISCO (SAI, 2 slots).
		 * DMIC: L = slot 1, R = slot 0 (swapped in SAI frame).
		 * Override via cfg->i2s.slot_mask if needed. */
		ctx->slot_count = 2;
		ctx->rx_slots[0] = 1; /* mono/left channel from slot 1 */
		ctx->rx_slots[1] = 0; /* right channel from slot 0 */

		return ove_audio_graph_add_node(g, &i2s_source_ops, ctx, name,
						OVE_AUDIO_NODE_SOURCE);
	}
#endif
	return OVE_ERR_NOT_SUPPORTED;
}

int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
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
		ctx->thread_priority = cfg->thread_priority ? cfg->thread_priority
							    : DEFAULT_AUDIO_PRIORITY;
		ctx->thread_stack_size = cfg->thread_stack_size ? cfg->thread_stack_size
								: DEFAULT_AUDIO_STACK;

		/* TX slot mapping: HP DAC L = slot 0, R = slot 1 */
		ctx->tx_slot_count = 2;
		ctx->tx_slots[0] = 0; /* mono/left to slot 0 */
		ctx->tx_slots[1] = 1; /* right to slot 1 */

		/* Create I2S bus.
		 * DMA buffer holds frames_per_period * slot_count * 2 samples
		 * (slot_count=2 for SAI, *2 for double-buffering). */
		unsigned int slot_count = 2;
		struct ove_i2s_cfg i2s_cfg = {
			.instance = 1, /* SAI2 */
			.sample_rate = cfg->fmt.sample_rate,
			.bit_depth = 16,
			.channels = cfg->fmt.channels,
			.direction = OVE_I2S_DIR_TXRX,
			.dma_buf_samples = g->frames_per_period * slot_count * 2,
		};

		/* DMA buffers MUST be in non-cacheable memory (DTCM on Cortex-M7).
		 * Heap is in cached SRAM — cannot use ove_i2s_create() here.
		 * Board linker script places .RxBUF / .TxBUF in DTCM. */
		static uint8_t tx_dma[4096] __attribute__((section(".TxBUF"), aligned(32)));
		static uint8_t rx_dma[4096] __attribute__((section(".RxBUF"), aligned(32)));
#ifndef CONFIG_OVE_ZERO_HEAP
		static ove_i2s_storage_t i2s_stor;
		int ret = ove_i2s_init(&ctx->i2s, &i2s_stor, tx_dma, rx_dma, &i2s_cfg);
#else
		int ret = ove_i2s_init(&ctx->i2s, &ctx->i2s_storage, tx_dma, rx_dma, &i2s_cfg);
#endif
		if (ret != OVE_OK)
			return ret;

		/* Board-specific codec init (WM8994 etc.) */
		ove_board_audio_codec_init(cfg->fmt.sample_rate, cfg->i2s.input_device);

		/* Register ISR callbacks with sink context as user_data */
		ove_i2s_set_tx_callback(ctx->i2s, audio_tx_complete_callback, ctx);
		ove_i2s_set_rx_callback(ctx->i2s, audio_rx_complete_callback, ctx);

		int idx =
			ove_audio_graph_add_node(g, &i2s_sink_ops, ctx, name, OVE_AUDIO_NODE_SINK);
		return idx;
	}
#endif
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_AUDIO */
