/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include <stdlib.h>
#include <string.h>

static struct {
	ove_audio_process_fn process_fn;
	void *user_data;
	unsigned int frames_per_buffer;
	unsigned int channels;
	ove_thread_t thread;
	volatile int running;
	int initialized;
} audio_state;

static void audio_loop(void *arg)
{
	(void)arg;
	size_t buf_samples = audio_state.frames_per_buffer *
			     audio_state.channels;
	int16_t *out = calloc(buf_samples, sizeof(int16_t));
	int16_t *in = calloc(buf_samples, sizeof(int16_t));

	while (audio_state.running) {
		if (audio_state.process_fn) {
			audio_state.process_fn(out, in,
					       audio_state.frames_per_buffer,
					       audio_state.user_data);
		}
		ove_thread_sleep_ms(10); /* ~10ms between callbacks */
	}

	free(out);
	free(in);
}

int ove_audio_init(const struct ove_audio_config *cfg,
		       ove_audio_process_fn fn, void *user_data)
{
	if (!cfg || !fn) {
		return OVE_ERR_INVALID_PARAM;
	}
	audio_state.process_fn = fn;
	audio_state.user_data = user_data;
	audio_state.frames_per_buffer = cfg->frames_per_buffer > 0
					       ? cfg->frames_per_buffer
					       : 256;
	audio_state.channels = cfg->channels > 0 ? cfg->channels : 2;
	audio_state.initialized = 1;
	return OVE_OK;
}

int ove_audio_start(void)
{
	if (!audio_state.initialized) {
		return OVE_ERR_NOT_REGISTERED;
	}
	if (audio_state.running) {
		return OVE_OK;
	}
	audio_state.running = 1;

#ifdef CONFIG_OVE_ZERO_HEAP
	static ove_thread_storage_t audio_th_storage;
	static uint8_t audio_th_stack[4096];
#endif

	struct ove_thread_desc desc = {
		.name = "audio_stub",
		.entry = audio_loop,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
		.stack_size = 4096,
#ifdef CONFIG_OVE_ZERO_HEAP
		.stack = audio_th_stack,
#endif
	};

#ifdef CONFIG_OVE_ZERO_HEAP
	if (ove_thread_init(&audio_state.thread, &audio_th_storage, &desc) != OVE_OK) {
#else
	if (ove_thread_create_(&audio_state.thread, &desc) != OVE_OK) {
#endif
		audio_state.running = 0;
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

int ove_audio_stop(void)
{
	if (!audio_state.running) {
		return OVE_OK;
	}
	audio_state.running = 0;
	if (audio_state.thread) {
		/* Let the audio thread see running==0, exit its loop, and
		 * reach vTaskSuspend before we call vTaskDelete.  Without
		 * this the POSIX-port vTaskDelete hits a live pthread. */
		ove_thread_sleep_ms(15);
#ifdef CONFIG_OVE_ZERO_HEAP
		ove_thread_deinit(audio_state.thread);
#else
		ove_thread_destroy(audio_state.thread);
#endif
		audio_state.thread = NULL;
	}
	return OVE_OK;
}

int ove_audio_pause(void)
{
	return ove_audio_stop();
}

int ove_audio_resume(void)
{
	return ove_audio_start();
}

void ove_audio_deinit(void)
{
	ove_audio_stop();
	memset(&audio_state, 0, sizeof(audio_state));
}
