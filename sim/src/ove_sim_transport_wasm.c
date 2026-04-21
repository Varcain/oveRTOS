/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM/Emscripten transport for the simulation framework.
 *
 * Sends events from the firmware (Web Worker) to the main browser
 * thread via postMessage with ArrayBuffer transfer.  Receives
 * commands from the dashboard via a mutex-protected queue.
 *
 * The dashboard JS calls ove_sim_wasm_push_cmd() (exported via
 * Emscripten ccall) to inject commands into the queue.
 */

#ifdef __EMSCRIPTEN__

#include "ove/sim/ove_sim_transport.h"
#include "ove/types.h"

#include <emscripten.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ── Command queue (dashboard -> firmware) ─────────────────────────── */

#define CMD_QUEUE_SIZE 32
#define CMD_MAX_LEN    256

struct cmd_entry {
	uint8_t  data[CMD_MAX_LEN];
	uint16_t len;
};

static struct cmd_entry cmd_queue[CMD_QUEUE_SIZE];
static int              cmd_head;
static int              cmd_tail;
static int              cmd_count;
static pthread_mutex_t  cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   cmd_cond = PTHREAD_COND_INITIALIZER;

/**
 * Push a command into the queue.  Called from the main browser thread
 * via Emscripten's ccall/cwrap (exported function).
 */
EMSCRIPTEN_KEEPALIVE
void ove_sim_wasm_push_cmd(const uint8_t *data, int len)
{
	if (len <= 0 || len > CMD_MAX_LEN)
		return;

	pthread_mutex_lock(&cmd_lock);
	if (cmd_count < CMD_QUEUE_SIZE) {
		struct cmd_entry *e = &cmd_queue[cmd_head];
		memcpy(e->data, data, (size_t)len);
		e->len = (uint16_t)len;
		cmd_head = (cmd_head + 1) % CMD_QUEUE_SIZE;
		cmd_count++;
		pthread_cond_signal(&cmd_cond);
	}
	pthread_mutex_unlock(&cmd_lock);
}

/* ── Transport ops ─────────────────────────────────────────────────── */

static int wasm_open(struct ove_sim_transport *t, const char *endpoint)
{
	(void)t;
	(void)endpoint;
	return OVE_OK;
}

static void wasm_close(struct ove_sim_transport *t)
{
	(void)t;
}

static int wasm_send_event(struct ove_sim_transport *t,
			   const struct ove_sim_event *event)
{
	(void)t;
	if (!event)
		return OVE_ERR_INVALID_PARAM;

	size_t total = sizeof(*event) + event->data_len;

	/*
	 * Deliver to the main-thread dashboard via a synchronous proxy
	 * call.  Raw worker postMessage is intercepted by Emscripten's
	 * pthread message router and not surfaced to the page.  The SYNC
	 * variant runs the JS on the main thread while this pthread
	 * blocks — necessary because callers (e.g. sim_debug) typically
	 * pass a pointer to a stack-allocated `struct ove_sim_event`, so
	 * the copy MUST happen before we return.  Debug events at ~2 Hz
	 * make the blocking cost negligible.
	 */
	MAIN_THREAD_EM_ASM({
		var len = $1;
		if (typeof window === 'undefined' ||
		    typeof window.__ove_sim_event !== 'function')
			return;
		var copy = new Uint8Array(len);
		copy.set(HEAPU8.subarray($0, $0 + len));
		window.__ove_sim_event(copy.buffer);
	}, (uintptr_t)event, (int)total);

	return OVE_OK;
}

static int wasm_recv_cmd(struct ove_sim_transport *t,
			 struct ove_sim_cmd *cmd, size_t cmd_size,
			 uint32_t timeout_ms)
{
	(void)t;

	pthread_mutex_lock(&cmd_lock);

	while (cmd_count == 0) {
		if (timeout_ms == 0) {
			pthread_mutex_unlock(&cmd_lock);
			return OVE_ERR_TIMEOUT;
		}

		if (timeout_ms == UINT32_MAX) {
			pthread_cond_wait(&cmd_cond, &cmd_lock);
		} else {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout_ms / 1000;
			ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			if (pthread_cond_timedwait(&cmd_cond, &cmd_lock,
						   &ts) != 0) {
				pthread_mutex_unlock(&cmd_lock);
				return OVE_ERR_TIMEOUT;
			}
		}
	}

	struct cmd_entry *e = &cmd_queue[cmd_tail];
	size_t copy_len = e->len < cmd_size ? e->len : cmd_size;
	memcpy(cmd, e->data, copy_len);
	cmd_tail = (cmd_tail + 1) % CMD_QUEUE_SIZE;
	cmd_count--;

	pthread_mutex_unlock(&cmd_lock);
	return OVE_OK;
}

/* ── Display / audio ops (WASM shared memory) ─────────────────────── */

#include "ove_sim_wasm_fb.h"
#include "ove_sim_wasm_audio.h"

static int wasm_flush_display(struct ove_sim_transport *t,
			      const void *fb, size_t fb_len,
			      uint16_t x1, uint16_t y1,
			      uint16_t x2, uint16_t y2)
{
	(void)t; (void)x1; (void)y1; (void)x2; (void)y2;
	uint16_t w = x2 - x1 + 1;
	uint16_t h = y2 - y1 + 1;
	ove_sim_wasm_fb_write(fb, (uint32_t)fb_len, w, h);
	return OVE_OK;
}

static int fmt_set;

static int wasm_push_audio(struct ove_sim_transport *t,
			   const void *samples, size_t len,
			   uint32_t sample_rate, uint16_t channels,
			   uint16_t bit_depth)
{
	(void)t;
	if (!fmt_set) {
		extern void ove_wasm_audio_set_playback_fmt(
			uint32_t rate, uint16_t ch, uint16_t bits);
		ove_wasm_audio_set_playback_fmt(sample_rate, channels,
						bit_depth);
		fmt_set = 1;
	}
	ove_wasm_audio_playback_write(samples, (uint32_t)len);
	return OVE_OK;
}

static size_t wasm_pull_audio(struct ove_sim_transport *t,
			      void *samples, size_t len)
{
	(void)t;
	return ove_wasm_audio_capture_read(samples, (uint32_t)len);
}

static const struct ove_sim_transport_ops wasm_ops = {
	.open          = wasm_open,
	.close         = wasm_close,
	.send_event    = wasm_send_event,
	.recv_cmd      = wasm_recv_cmd,
	.flush_display = wasm_flush_display,
	.push_audio    = wasm_push_audio,
	.pull_audio    = wasm_pull_audio,
};

/* ── Command pump thread ──────────────────────────────────────────── */

#include "ove/sim/ove_sim_plugin.h"

static struct ove_sim_transport *pump_transport;

static void *cmd_pump_thread(void *arg)
{
	(void)arg;
	struct ove_sim_cmd cmd;

	while (1) {
		int ret = ove_sim_transport_recv_cmd(pump_transport,
						     &cmd, sizeof(cmd), 1000);
		if (ret == OVE_OK)
			ove_sim_plugin_dispatch_cmd(&cmd);
	}
	return NULL;
}

int ove_sim_wasm_cmd_pump_start(struct ove_sim_transport *t)
{
	pump_transport = t;
	pthread_t th;
	return pthread_create(&th, NULL, cmd_pump_thread, NULL);
}

/* ── Public factory ────────────────────────────────────────────────── */

int ove_sim_transport_wasm_create(struct ove_sim_transport *t)
{
	if (!t)
		return OVE_ERR_INVALID_PARAM;

	t->ops = &wasm_ops;
	t->priv = NULL;
	return OVE_OK;
}

#endif /* __EMSCRIPTEN__ */
