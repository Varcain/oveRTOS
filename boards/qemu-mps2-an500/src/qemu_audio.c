/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU audio driver — streams PCM through /dev/shm/ove-audio via ARM
 * semihosting.  The host-side viewer (qemu-display-viewer.py) mmaps
 * the same file for SDL2 audio playback and capture.
 *
 * Falls back to silent in-memory processing when the shared-memory
 * file cannot be opened (headless / no audio viewer).
 */

#include "ove/ove.h"
#include "semihosting.h"
#include "qemu_audio_shm.h"
#include <stdlib.h>
#include <string.h>

static struct {
	ove_audio_process_fn process_fn;
	void *user_data;
	unsigned int sample_rate;
	unsigned int frames_per_buffer;
	unsigned int channels;
	unsigned int bit_depth;
	ove_thread_t thread;
	volatile int running;
	int initialized;
	int sh_fd;           /* semihosting fd, <0 = headless */
	uint32_t out_wpos;   /* local copy of output write position */
	uint32_t in_rpos;    /* local copy of input read position */
} audio_state;

/* ------------------------------------------------------------------ */
/* Ringbuffer helpers                                                   */
/* ------------------------------------------------------------------ */

/* Write `len` bytes from `buf` into the output ring, wrapping at end. */
static void ring_write_out(const void *buf, uint32_t len)
{
	int fd = audio_state.sh_fd;
	uint32_t wpos = audio_state.out_wpos;
	uint32_t ring_off = AUDIO_SHM_OUT_RING_OFF;
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
	audio_state.out_wpos = wpos + len;
}

/* Read `len` bytes from the input ring into `buf`, wrapping at end.
 * Returns the number of bytes actually available (may be < len). */
static uint32_t ring_read_in(void *buf, uint32_t len)
{
	int fd = audio_state.sh_fd;
	uint32_t rpos = audio_state.in_rpos;
	uint32_t ring_off = AUDIO_SHM_IN_RING_OFF;
	uint32_t mask = AUDIO_SHM_RING_SIZE - 1;

	/* Read host's current in_write_pos from header */
	uint32_t in_wpos;
	sh_seek(fd, offsetof(struct audio_shm_header, in_write_pos));
	sh_read(fd, &in_wpos, sizeof(in_wpos));

	uint32_t avail = in_wpos - rpos;
	if (avail > AUDIO_SHM_RING_SIZE)
		avail = 0; /* underflow — host hasn't written yet */
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
	audio_state.in_rpos = rpos + len;
	return len;
}

/* Flush local positions back to the shared header. */
static void flush_positions(void)
{
	int fd = audio_state.sh_fd;

	sh_seek(fd, offsetof(struct audio_shm_header, out_write_pos));
	sh_write(fd, &audio_state.out_wpos, sizeof(uint32_t));

	sh_seek(fd, offsetof(struct audio_shm_header, in_read_pos));
	sh_write(fd, &audio_state.in_rpos, sizeof(uint32_t));
}

/* ------------------------------------------------------------------ */
/* Audio processing thread                                              */
/* ------------------------------------------------------------------ */

static void audio_loop(void *arg)
{
	(void)arg;

	size_t buf_samples = audio_state.frames_per_buffer *
			     audio_state.channels;
	size_t buf_bytes = buf_samples * (audio_state.bit_depth / 8);

#ifdef CONFIG_OVE_ZERO_HEAP
	static int16_t out_buf[1024];
	static int16_t in_buf[1024];
#else
	int16_t *out_buf = calloc(buf_samples, sizeof(int16_t));
	int16_t *in_buf = calloc(buf_samples, sizeof(int16_t));
#endif

	/*
	 * Pacing strategy: produce N buffers in a batch, then check
	 * if the ring is getting full.  This amortises the semihosting
	 * overhead of reading the host's read position over many
	 * buffers instead of checking every single one.
	 *
	 * When the ring is above 50%, sleep and re-check with a long
	 * virtual sleep (50ms) to reduce polling overhead while
	 * waiting for the host to drain.
	 */
	unsigned int batch = 0;
	const unsigned int batch_size = 8; /* check every 8 buffers */

	while (audio_state.running) {
		if (audio_state.process_fn) {
			/* Read input from host (or keep silence) */
			if (audio_state.sh_fd >= 0) {
				uint32_t got = ring_read_in(in_buf, buf_bytes);
				if (got < buf_bytes)
					memset((uint8_t *)in_buf + got, 0,
					       buf_bytes - got);
			}

			audio_state.process_fn(out_buf, in_buf,
					       audio_state.frames_per_buffer,
					       audio_state.user_data);

			/* Write output to host */
			if (audio_state.sh_fd >= 0) {
				ring_write_out(out_buf, buf_bytes);
				flush_positions();
			}
		}

		batch++;

		if (audio_state.sh_fd >= 0 && batch >= batch_size) {
			batch = 0;
			/* Check ring fill level */
			uint32_t host_rpos;
			sh_seek(audio_state.sh_fd,
				offsetof(struct audio_shm_header,
					 out_read_pos));
			sh_read(audio_state.sh_fd, &host_rpos,
				sizeof(host_rpos));
			uint32_t buffered =
				audio_state.out_wpos - host_rpos;

			/* If ring is more than half full, sleep to let
			 * the host catch up.  Use a long virtual sleep
			 * to minimise semihosting polling overhead. */
			while (buffered > AUDIO_SHM_RING_SIZE / 2
			       && audio_state.running) {
				ove_thread_sleep_ms(50);
				sh_seek(audio_state.sh_fd,
					offsetof(struct audio_shm_header,
						 out_read_pos));
				sh_read(audio_state.sh_fd, &host_rpos,
					sizeof(host_rpos));
				buffered = audio_state.out_wpos
					   - host_rpos;
			}
		} else if (audio_state.sh_fd < 0) {
			unsigned int period_ms =
				(audio_state.frames_per_buffer * 1000)
				/ audio_state.sample_rate;
			if (period_ms < 1)
				period_ms = 1;
			ove_thread_sleep_ms(period_ms);
		}
	}

#ifndef CONFIG_OVE_ZERO_HEAP
	free(out_buf);
	free(in_buf);
#endif
}

/* ------------------------------------------------------------------ */
/* ove_audio API                                                        */
/* ------------------------------------------------------------------ */

int ove_audio_init(const struct ove_audio_config *cfg,
		       ove_audio_process_fn fn, void *user_data)
{
	if (!cfg || !fn)
		return OVE_ERR_INVALID_PARAM;

	audio_state.process_fn = fn;
	audio_state.user_data = user_data;
	audio_state.sample_rate = cfg->sample_rate > 0
					  ? cfg->sample_rate : 48000;
	audio_state.frames_per_buffer = cfg->frames_per_buffer > 0
						? cfg->frames_per_buffer : 256;
	audio_state.channels = cfg->channels > 0 ? cfg->channels : 2;
	audio_state.bit_depth = cfg->bit_depth > 0 ? cfg->bit_depth : 16;

	/* Try to open shared-memory audio file via semihosting.
	 * mode 7 = "r+b" — file must already exist (created by qemu-run.sh). */
	audio_state.sh_fd = sh_open(AUDIO_SHM_PATH, 7);

	if (audio_state.sh_fd >= 0) {
		/* Write header so host viewer knows our format */
		struct audio_shm_header hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = AUDIO_SHM_MAGIC;
		hdr.sample_rate = audio_state.sample_rate;
		hdr.channels = audio_state.channels;
		hdr.bit_depth = audio_state.bit_depth;
		hdr.frames_per_buffer = audio_state.frames_per_buffer;
		hdr.ring_size = AUDIO_SHM_RING_SIZE;

		sh_seek(audio_state.sh_fd, 0);
		sh_write(audio_state.sh_fd, &hdr, sizeof(hdr));
	}

	audio_state.out_wpos = 0;
	audio_state.in_rpos = 0;
	audio_state.initialized = 1;
	return OVE_OK;
}

int ove_audio_start(void)
{
	if (!audio_state.initialized)
		return OVE_ERR_NOT_REGISTERED;
	if (audio_state.running)
		return OVE_OK;

	audio_state.running = 1;

#ifdef CONFIG_OVE_ZERO_HEAP
	static ove_thread_storage_t audio_th_storage;
	static uint8_t audio_th_stack[4096];
#endif

	struct ove_thread_desc desc = {
		.name = "qemu_audio",
		.entry = audio_loop,
		.arg = NULL,
		.priority = OVE_PRIO_NORMAL,
		.stack_size = 4096,
#ifdef CONFIG_OVE_ZERO_HEAP
		.stack = audio_th_stack,
#endif
	};

#ifdef CONFIG_OVE_ZERO_HEAP
	if (ove_thread_init(&audio_state.thread, &audio_th_storage,
			    &desc) != OVE_OK) {
#else
	if (ove_thread_create(&audio_state.thread, &desc) != OVE_OK) {
#endif
		audio_state.running = 0;
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

int ove_audio_stop(void)
{
	if (!audio_state.running)
		return OVE_OK;

	audio_state.running = 0;
	if (audio_state.thread) {
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
	if (audio_state.sh_fd >= 0) {
		sh_close(audio_state.sh_fd);
		audio_state.sh_fd = -1;
	}
	memset(&audio_state, 0, sizeof(audio_state));
	audio_state.sh_fd = -1;
}
