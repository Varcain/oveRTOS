/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/audio.h"
#include "ove_backend_common.h"
#include "FreeRTOS.h"
#include "task.h"

#include "i2s_da.h"
#include "i2s_stm32f7.h"
#include "audio_codec_da.h"

#include <string.h>

/* ========================================================================= */
/* INTERNAL STATE                                                            */
/* ========================================================================= */

typedef enum {
	BUFFER_FIRST_HALF = 0,
	BUFFER_SECOND_HALF = 1
} buffer_phase_t;

#define DEFAULT_AUDIO_PRIORITY  (tskIDLE_PRIORITY + 7)
#define DEFAULT_AUDIO_STACK     (configMINIMAL_STACK_SIZE * 32)

static ove_audio_process_fn g_process_fn;
static void *g_user_data;
static unsigned int g_frames_per_buffer;
static unsigned int g_thread_priority;
static unsigned int g_thread_stack_size;

static TaskHandle_t audio_task_handle;
static volatile buffer_phase_t current_rx_phase;
static volatile buffer_phase_t current_tx_phase;

/* ========================================================================= */
/* ISR CALLBACKS                                                             */
/* ========================================================================= */

static void audio_rx_complete_callback(void)
{
	BaseType_t yield_required = pdFALSE;

	if (audio_task_handle == NULL) {
		return;
	}

	uint8_t completed_rx_half = i2s_stm32f7_getRxCompletedBufferHalf();
	current_rx_phase = (buffer_phase_t)completed_rx_half;

	vTaskNotifyGiveFromISR(audio_task_handle, &yield_required);
	portYIELD_FROM_ISR(yield_required);
}

static void audio_tx_complete_callback(void)
{
	if (audio_task_handle == NULL) {
		return;
	}

	uint8_t completed_tx_half = i2s_stm32f7_getTxCompletedBufferHalf();
	current_tx_phase = (buffer_phase_t)completed_tx_half;

	/* Do NOT notify the task here — only the RX callback drives the audio
	 * task.  TX ISR fires at higher NVIC priority than RX, so by the time
	 * the RX callback notifies the task, tx_phase is already updated.
	 * Notifying from both callbacks caused a PendSV race: PendSV preempted
	 * the RX ISR, waking the task with only TX updated → perpetual skip. */
}

/* ========================================================================= */
/* AUDIO PROCESSING TASK                                                     */
/* ========================================================================= */

static void audio_task_fn(void *pvParameters)
{
	(void)pvParameters;

	buffer_phase_t last_rx_phase = BUFFER_SECOND_HALF;
	buffer_phase_t last_tx_phase = BUFFER_SECOND_HALF;

	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

		buffer_phase_t process_rx_phase = current_rx_phase;
		buffer_phase_t process_tx_phase = current_tx_phase;

		int skip = 0;
		if (process_tx_phase == last_tx_phase) {
			skip = 1;
		}
		last_tx_phase = process_tx_phase;

		if (process_rx_phase == last_rx_phase) {
			skip = 1;
		}
		last_rx_phase = process_rx_phase;
		if (skip) {
			continue;
		}

		int16_t *rx_ptr = (int16_t *)i2s_stm32f7_getRxBuffer();
		int16_t *tx_ptr = (int16_t *)i2s_stm32f7_getTxBuffer();

		if (g_process_fn != NULL) {
			g_process_fn(tx_ptr, rx_ptr, g_frames_per_buffer,
				     g_user_data);
		}
	}
}

/* ========================================================================= */
/* OPS IMPLEMENTATION                                                        */
/* ========================================================================= */

int ove_audio_init(const struct ove_audio_config *cfg,
			       ove_audio_process_fn fn, void *user_data)
{
	if (cfg == NULL || fn == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	g_process_fn = fn;
	g_user_data = user_data;
	g_frames_per_buffer = cfg->frames_per_buffer;
	g_thread_priority = cfg->thread_priority ? cfg->thread_priority
						 : DEFAULT_AUDIO_PRIORITY;
	g_thread_stack_size = cfg->thread_stack_size ? cfg->thread_stack_size
						     : DEFAULT_AUDIO_STACK;

	/* Register and initialize I2S driver */
	if (!i2s_set_driver(i2s_stm32f7_get())) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	i2s_stm32f7_setTxCompleteCb(audio_tx_complete_callback);
	i2s_setRxCompleteCb(audio_rx_complete_callback);
	i2s_init();

	return OVE_OK;
}

int ove_audio_start(void)
{
	BaseType_t ret;

	/* Create the audio processing task */
	ret = xTaskCreate(audio_task_fn, "Audio",
			  g_thread_stack_size, NULL,
			  g_thread_priority, &audio_task_handle);
	if (ret != pdPASS) {
		return OVE_ERR_NO_MEMORY;
	}

	i2s_startStream();
	return OVE_OK;
}

int ove_audio_stop(void)
{
	i2s_pauseStream();
	return OVE_OK;
}

int ove_audio_pause(void)
{
	i2s_pauseStream();
	return OVE_OK;
}

int ove_audio_resume(void)
{
	i2s_resumeStream();
	return OVE_OK;
}

void ove_audio_deinit(void)
{
	if (audio_task_handle != NULL) {
		vTaskDelete(audio_task_handle);
		audio_task_handle = NULL;
	}
}
