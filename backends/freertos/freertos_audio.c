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

#include "i2s_da.h"
#include "i2s_stm32f7.h"
#include "audio_codec_da.h"

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

/* ========================================================================= */
/* I2S SOURCE NODE                                                           */
/* ========================================================================= */

struct i2s_source_ctx {
	struct ove_audio_fmt fmt;
	unsigned int         input_device; /* 0=line-in, 1=dmic */
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
	/* Copy from the I2S RX DMA buffer into the graph buffer.
	 * The engine task calls this after being notified by the RX ISR,
	 * so the DMA pointer is valid and stable for this half. */
	int16_t *rx_ptr = (int16_t *)i2s_stm32f7_getRxBuffer();
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

struct i2s_sink_ctx {
	struct ove_audio_fmt    fmt;
	struct ove_audio_graph *graph;
	unsigned int            frames_per_period;
	unsigned int            thread_priority;
	unsigned int            thread_stack_size;
	TaskHandle_t            task_handle;
	volatile buffer_phase_t current_rx_phase;
	volatile buffer_phase_t current_tx_phase;
#ifdef CONFIG_OVE_ZERO_HEAP
	StaticTask_t            task_tcb;
	StackType_t             task_stack[DEFAULT_AUDIO_STACK];
#endif
};

/* Single static sink context — only one I2S graph can be active at a time */
static struct i2s_sink_ctx g_sink_ctx;

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
	(void)ctx;
	(void)out;
	/* Copy processed audio into the I2S TX DMA buffer */
	int16_t *tx_ptr = (int16_t *)i2s_stm32f7_getTxBuffer();
	unsigned int bytes = in->frames * in->fmt->channels *
			     ove_audio_sample_size(in->fmt->sample_fmt);
	memcpy(tx_ptr, in->data, bytes);
	return OVE_OK;
}

/* ── ISR callbacks ──────────────────────────────────────────────── */

static void audio_rx_complete_callback(void)
{
	if (g_sink_ctx.task_handle == NULL)
		return;

	uint8_t completed_rx_half = i2s_stm32f7_getRxCompletedBufferHalf();
	g_sink_ctx.current_rx_phase = (buffer_phase_t)completed_rx_half;

	BaseType_t yield_required = pdFALSE;
	vTaskNotifyGiveFromISR(g_sink_ctx.task_handle, &yield_required);
	portYIELD_FROM_ISR(yield_required);
}

static void audio_tx_complete_callback(void)
{
	if (g_sink_ctx.task_handle == NULL)
		return;

	uint8_t completed_tx_half = i2s_stm32f7_getTxCompletedBufferHalf();
	g_sink_ctx.current_tx_phase = (buffer_phase_t)completed_tx_half;

	/* Do NOT notify the task here — only the RX callback drives the audio
	 * task.  TX ISR fires at higher NVIC priority than RX, so by the time
	 * the RX callback notifies the task, tx_phase is already updated.
	 * Notifying from both callbacks caused a PendSV race: PendSV preempted
	 * the RX ISR, waking the task with only TX updated → perpetual skip. */
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

		/* Walk the entire graph: source reads DMA RX, processors
		 * transform, sink writes DMA TX */
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

	i2s_startStream();
	return OVE_OK;
}

static int i2s_sink_stop(void *ctx)
{
	struct i2s_sink_ctx *sc = (struct i2s_sink_ctx *)ctx;

	i2s_pauseStream();

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

/* ========================================================================= */
/* DEVICE NODE FACTORIES                                                     */
/* ========================================================================= */

int ove_audio_device_source(struct ove_audio_graph *g,
			    const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	/* Use static context embedded in g_sink_ctx — there's only one I2S.
	 * Source context is lightweight, allocate on heap. */
	struct i2s_source_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;

	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->input_device = cfg->i2s.input_device;

	/* Configure I2S hardware */
	if (!i2s_set_driver(i2s_stm32f7_get()))
		return OVE_ERR_NOT_SUPPORTED;

	i2s_stm32f7_set_input_device(ctx->input_device ? 1 : 0);
	if (cfg->fmt.sample_rate)
		i2s_stm32f7_set_sample_rate(cfg->fmt.sample_rate);

	int idx = ove_audio_graph_add_node(g, &i2s_source_ops, ctx, name,
					   OVE_AUDIO_NODE_SOURCE);
	if (idx < 0)
		OVE_BACKEND_FREE(ctx);
	return idx;
}

int ove_audio_device_sink(struct ove_audio_graph *g,
			  const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name)
		return OVE_ERR_INVALID_PARAM;

	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S)
		return OVE_ERR_NOT_SUPPORTED;

	/* Use static sink context (one I2S sink at a time) */
	struct i2s_sink_ctx *ctx = &g_sink_ctx;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->graph = g;
	ctx->frames_per_period = g->frames_per_period;
	ctx->thread_priority = cfg->thread_priority ? cfg->thread_priority
						    : DEFAULT_AUDIO_PRIORITY;
	ctx->thread_stack_size = cfg->thread_stack_size ? cfg->thread_stack_size
							: DEFAULT_AUDIO_STACK;

	/* Register ISR callbacks */
	i2s_stm32f7_setTxCompleteCb(audio_tx_complete_callback);
	i2s_setRxCompleteCb(audio_rx_complete_callback);
	i2s_init();

	int idx = ove_audio_graph_add_node(g, &i2s_sink_ops, ctx, name,
					   OVE_AUDIO_NODE_SINK);
	return idx;
}

#endif /* CONFIG_OVE_AUDIO */
