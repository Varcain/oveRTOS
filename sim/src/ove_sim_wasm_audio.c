/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifdef __EMSCRIPTEN__

#include "ove_sim_wasm_audio.h"
#include <emscripten.h>
#include <string.h>

/* Global audio state in WASM heap (SharedArrayBuffer). */
struct ove_sim_wasm_audio ove_wasm_audio;

/* ── Ring buffer helpers (SPSC — no lock needed) ───────────────────── */
/*
 * Use __atomic builtins for write_pos/read_pos so that cross-thread
 * updates between the JS main thread (Atomics.store) and the WASM
 * pthread are guaranteed visible.  volatile alone is NOT sufficient
 * on WASM SharedArrayBuffer.
 */

#define RING_MASK (OVE_SIM_WASM_AUDIO_RING_SIZE - 1)

static uint32_t ring_load_wp(const struct ove_sim_wasm_audio_ring *r)
{
	return __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
}

static uint32_t ring_load_rp(const struct ove_sim_wasm_audio_ring *r)
{
	return __atomic_load_n(&r->read_pos, __ATOMIC_ACQUIRE);
}

static uint32_t ring_avail(const struct ove_sim_wasm_audio_ring *r)
{
	return ring_load_wp(r) - ring_load_rp(r);
}

static uint32_t ring_free(const struct ove_sim_wasm_audio_ring *r)
{
	return OVE_SIM_WASM_AUDIO_RING_SIZE - ring_avail(r);
}

static void ring_write(struct ove_sim_wasm_audio_ring *r,
		       const uint8_t *data, uint32_t len)
{
	uint32_t wp = ring_load_wp(r);
	for (uint32_t i = 0; i < len; i++)
		r->buf[(wp + i) & RING_MASK] = data[i];
	__atomic_store_n(&r->write_pos, wp + len, __ATOMIC_RELEASE);
}

static uint32_t ring_read(struct ove_sim_wasm_audio_ring *r,
			   uint8_t *data, uint32_t len)
{
	uint32_t wp = ring_load_wp(r);
	uint32_t rp = ring_load_rp(r);
	uint32_t avail = wp - rp;
	if (len > avail) len = avail;
	for (uint32_t i = 0; i < len; i++)
		data[i] = r->buf[(rp + i) & RING_MASK];
	__atomic_store_n(&r->read_pos, rp + len, __ATOMIC_RELEASE);
	return len;
}

/* ── Exported accessors for JS ─────────────────────────────────────── */

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_audio_get_playback_ptr(void)
{
	return &ove_wasm_audio.playback;
}

EMSCRIPTEN_KEEPALIVE
void *ove_wasm_audio_get_capture_ptr(void)
{
	return &ove_wasm_audio.capture;
}

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_audio_playback_available(void)
{
	return ring_load_wp(&ove_wasm_audio.playback) -
	       ring_load_rp(&ove_wasm_audio.playback);
}

EMSCRIPTEN_KEEPALIVE
uint32_t ove_wasm_audio_capture_available(void)
{
	return ring_load_wp(&ove_wasm_audio.capture) -
	       ring_load_rp(&ove_wasm_audio.capture);
}

EMSCRIPTEN_KEEPALIVE
void ove_wasm_audio_set_capture_fmt(uint32_t rate, uint16_t ch, uint16_t bits)
{
	ove_wasm_audio.capture.sample_rate = rate;
	ove_wasm_audio.capture.channels = ch;
	ove_wasm_audio.capture.bit_depth = bits;
}

EMSCRIPTEN_KEEPALIVE
void ove_wasm_audio_set_playback_fmt(uint32_t rate, uint16_t ch, uint16_t bits)
{
	ove_wasm_audio.playback.sample_rate = rate;
	ove_wasm_audio.playback.channels = ch;
	ove_wasm_audio.playback.bit_depth = bits;
}

/* ── C-side API (called from sim_audio.c graph nodes) ──────────────── */

void ove_wasm_audio_playback_write(const void *samples, uint32_t len)
{
	if (ring_free(&ove_wasm_audio.playback) < len)
		return; /* Drop if buffer full — JS not reading fast enough */
	ring_write(&ove_wasm_audio.playback, (const uint8_t *)samples, len);
}

size_t ove_wasm_audio_capture_read(void *samples, uint32_t len)
{
	uint32_t got = ring_read(&ove_wasm_audio.capture,
				 (uint8_t *)samples, len);
	/* Zero-fill remainder if mic hasn't provided enough. */
	if (got < len)
		memset((uint8_t *)samples + got, 0, len - got);
	return got;
}

#endif /* __EMSCRIPTEN__ */
