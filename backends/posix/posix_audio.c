/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/audio.h"
#include "ove_backend_common.h"
#include <SDL.h>
#include <string.h>

static struct {
	ove_audio_process_fn process_fn;
	void *user_data;
	unsigned int frames_per_buffer;
	unsigned int channels;
	unsigned int sample_rate;
	SDL_AudioDeviceID out_dev;
	SDL_AudioDeviceID in_dev;
	int initialized;
	int16_t *capture_buf;
	size_t capture_buf_samples;
} audio_ctx;

static void audio_output_callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;
	int samples = len / (int)sizeof(int16_t);
	int frames = samples / (int)audio_ctx.channels;
	int16_t *out = (int16_t *)stream;

	/* Read captured input if available */
	if (audio_ctx.in_dev > 0) {
		Uint32 avail = SDL_GetQueuedAudioSize(audio_ctx.in_dev);
		if (avail >= (Uint32)len) {
			SDL_DequeueAudio(audio_ctx.in_dev,
					 audio_ctx.capture_buf, (Uint32)len);
		} else {
			memset(audio_ctx.capture_buf, 0, (size_t)len);
		}
	} else {
		memset(audio_ctx.capture_buf, 0, (size_t)len);
	}

	if (audio_ctx.process_fn) {
		audio_ctx.process_fn(out, audio_ctx.capture_buf,
				     (unsigned int)frames,
				     audio_ctx.user_data);
	} else {
		memset(stream, 0, (size_t)len);
	}
}

int ove_audio_init(const struct ove_audio_config *cfg,
		       ove_audio_process_fn fn, void *user_data)
{
	if (!cfg || !fn) {
		return OVE_ERR_INVALID_PARAM;
	}

	audio_ctx.process_fn = fn;
	audio_ctx.user_data = user_data;
	audio_ctx.frames_per_buffer = cfg->frames_per_buffer > 0
					      ? cfg->frames_per_buffer : 512;
	audio_ctx.channels = cfg->channels > 0 ? cfg->channels : 1;
	audio_ctx.sample_rate = cfg->sample_rate > 0
					? cfg->sample_rate : 44100;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	/* Allocate capture buffer */
	audio_ctx.capture_buf_samples = audio_ctx.frames_per_buffer *
					audio_ctx.channels;
	audio_ctx.capture_buf = OVE_BACKEND_MALLOC(
		audio_ctx.capture_buf_samples * sizeof(int16_t));
	if (!audio_ctx.capture_buf) {
		return OVE_ERR_NO_MEMORY;
	}
	memset(audio_ctx.capture_buf, 0,
	       audio_ctx.capture_buf_samples * sizeof(int16_t));

	/* Open output device */
	SDL_AudioSpec want_out, have_out;
	memset(&want_out, 0, sizeof(want_out));
	want_out.freq = (int)audio_ctx.sample_rate;
	want_out.format = AUDIO_S16SYS;
	want_out.channels = (Uint8)audio_ctx.channels;
	want_out.samples = (Uint16)audio_ctx.frames_per_buffer;
	want_out.callback = audio_output_callback;

	audio_ctx.out_dev = SDL_OpenAudioDevice(NULL, 0, &want_out,
						&have_out, 0);
	if (audio_ctx.out_dev == 0) {
		OVE_BACKEND_FREE(audio_ctx.capture_buf);
		audio_ctx.capture_buf = NULL;
		return OVE_ERR_NOT_SUPPORTED;
	}

	/* Try to open input device (not fatal if unavailable) */
	SDL_AudioSpec want_in, have_in;
	memset(&want_in, 0, sizeof(want_in));
	want_in.freq = (int)audio_ctx.sample_rate;
	want_in.format = AUDIO_S16SYS;
	want_in.channels = (Uint8)audio_ctx.channels;
	want_in.samples = (Uint16)audio_ctx.frames_per_buffer;
	want_in.callback = NULL; /* Use queue mode for capture */

	audio_ctx.in_dev = SDL_OpenAudioDevice(NULL, 1, &want_in,
					       &have_in, 0);
	/* in_dev == 0 is fine — we'll provide silence */

	audio_ctx.initialized = 1;
	return OVE_OK;
}

int ove_audio_start(void)
{
	if (!audio_ctx.initialized) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	SDL_PauseAudioDevice(audio_ctx.out_dev, 0);
	if (audio_ctx.in_dev > 0) {
		SDL_PauseAudioDevice(audio_ctx.in_dev, 0);
	}
	return OVE_OK;
}

int ove_audio_stop(void)
{
	if (audio_ctx.out_dev > 0) {
		SDL_PauseAudioDevice(audio_ctx.out_dev, 1);
	}
	if (audio_ctx.in_dev > 0) {
		SDL_PauseAudioDevice(audio_ctx.in_dev, 1);
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
	if (audio_ctx.out_dev > 0) {
		SDL_CloseAudioDevice(audio_ctx.out_dev);
		audio_ctx.out_dev = 0;
	}
	if (audio_ctx.in_dev > 0) {
		SDL_CloseAudioDevice(audio_ctx.in_dev);
		audio_ctx.in_dev = 0;
	}
	OVE_BACKEND_FREE(audio_ctx.capture_buf);
	audio_ctx.capture_buf = NULL;
	audio_ctx.initialized = 0;
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
