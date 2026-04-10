/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Common PCM ring buffer for simulation audio.
 *
 * Shared struct layout and helpers used by all three sim transport
 * backends (WASM, POSIX, QEMU).  The JS AudioWorklet accesses the
 * same layout by byte offset.
 *
 * Design:
 *   - SPSC (single producer, single consumer) — no lock needed
 *   - Power-of-2 size with wrapping positions (no modulo)
 *   - 32-byte header followed by PCM data
 *
 * Access modes:
 *   WASM:  direct memory with __atomic builtins (SharedArrayBuffer)
 *   POSIX: direct memory (in-process, mutex at transport level)
 *   QEMU:  semihosting file I/O using offset constants
 */

#ifndef OVE_SIM_AUDIO_RING_H
#define OVE_SIM_AUDIO_RING_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Default ring size ────────────────────────────────────────────────
 * 64 KB per direction.  At 16 kHz / 16-bit / mono = 32 kB/s ≈ 2 s.
 * At 48 kHz / 16-bit / stereo = 192 kB/s ≈ 340 ms.
 */
#define OVE_SIM_AUDIO_RING_SIZE  (1u << 16)  /* 65536 bytes */

/* ── Byte offsets (for JS AudioWorklet and QEMU semihosting) ──────── */

#define OVE_RING_OFF_WRITE_POS    0
#define OVE_RING_OFF_READ_POS     4
#define OVE_RING_OFF_SAMPLE_RATE  8
#define OVE_RING_OFF_CHANNELS    12
#define OVE_RING_OFF_BIT_DEPTH   14
#define OVE_RING_OFF_SIZE        16
#define OVE_RING_OFF_UNDERRUNS   20
#define OVE_RING_OFF_OVERRUNS    24
#define OVE_RING_OFF_BUF         32

/* ── Ring struct ──────────────────────────────────────────────────── */

struct ove_sim_audio_ring {
	uint32_t write_pos;     /*  0 — producer byte offset (wraps) */
	uint32_t read_pos;      /*  4 — consumer byte offset (wraps) */
	uint32_t sample_rate;   /*  8 — Hz */
	uint16_t channels;      /* 12 */
	uint16_t bit_depth;     /* 14 — bits per sample */
	uint32_t size;          /* 16 — ring capacity in bytes (power-of-2) */
	uint32_t underruns;     /* 20 — consumer found ring empty */
	uint32_t overruns;      /* 24 — producer found ring full */
	uint32_t _reserved;     /* 28 — pad header to 32 bytes */
	uint8_t  buf[OVE_SIM_AUDIO_RING_SIZE];  /* 32 — PCM data */
};

/* ── Inline helpers (direct memory access) ────────────────────────── */

static inline void ove_sim_audio_ring_init(struct ove_sim_audio_ring *r,
					   uint32_t sample_rate,
					   uint16_t channels,
					   uint16_t bit_depth)
{
	memset(r, 0, sizeof(*r));
	r->sample_rate = sample_rate;
	r->channels = channels;
	r->bit_depth = bit_depth;
	r->size = OVE_SIM_AUDIO_RING_SIZE;
}

static inline uint32_t ove_sim_ring_mask(const struct ove_sim_audio_ring *r)
{
	return r->size - 1;
}

static inline uint32_t ove_sim_ring_avail(const struct ove_sim_audio_ring *r)
{
	return r->write_pos - r->read_pos;
}

static inline uint32_t ove_sim_ring_free(const struct ove_sim_audio_ring *r)
{
	return r->size - ove_sim_ring_avail(r);
}

/** @return Fill level 0–100. */
static inline unsigned int
ove_sim_ring_fill_pct(const struct ove_sim_audio_ring *r)
{
	if (r->size == 0)
		return 0;
	return (unsigned int)((uint64_t)ove_sim_ring_avail(r) * 100 / r->size);
}

/**
 * Write @p len bytes into the ring.
 * @return Number of bytes actually written (may be less if ring full).
 */
static inline uint32_t ove_sim_ring_write(struct ove_sim_audio_ring *r,
					  const void *data, uint32_t len)
{
	uint32_t free = ove_sim_ring_free(r);
	if (len > free) {
		len = free;
		r->overruns++;
	}
	if (len == 0)
		return 0;

	uint32_t mask = ove_sim_ring_mask(r);
	uint32_t wp = r->write_pos;
	const uint8_t *src = (const uint8_t *)data;

	uint32_t pos = wp & mask;
	uint32_t first = r->size - pos;
	if (first >= len) {
		memcpy(r->buf + pos, src, len);
	} else {
		memcpy(r->buf + pos, src, first);
		memcpy(r->buf, src + first, len - first);
	}
	r->write_pos = wp + len;
	return len;
}

/**
 * Read up to @p len bytes from the ring.
 * @return Number of bytes actually read.
 */
static inline uint32_t ove_sim_ring_read(struct ove_sim_audio_ring *r,
					 void *data, uint32_t len)
{
	uint32_t avail = ove_sim_ring_avail(r);
	if (len > avail) {
		len = avail;
		if (avail == 0)
			r->underruns++;
	}
	if (len == 0)
		return 0;

	uint32_t mask = ove_sim_ring_mask(r);
	uint32_t rp = r->read_pos;
	uint8_t *dst = (uint8_t *)data;

	uint32_t pos = rp & mask;
	uint32_t first = r->size - pos;
	if (first >= len) {
		memcpy(dst, r->buf + pos, len);
	} else {
		memcpy(dst, r->buf + pos, first);
		memcpy(dst + first, r->buf, len - first);
	}
	r->read_pos = rp + len;
	return len;
}

/* ── Atomic variants (for WASM SharedArrayBuffer) ─────────────────── */

#ifdef __EMSCRIPTEN__

static inline uint32_t
ove_sim_ring_avail_atomic(const struct ove_sim_audio_ring *r)
{
	uint32_t wp = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
	uint32_t rp = __atomic_load_n(&r->read_pos, __ATOMIC_ACQUIRE);
	return wp - rp;
}

static inline uint32_t
ove_sim_ring_free_atomic(const struct ove_sim_audio_ring *r)
{
	return r->size - ove_sim_ring_avail_atomic(r);
}

static inline uint32_t
ove_sim_ring_write_atomic(struct ove_sim_audio_ring *r,
			  const void *data, uint32_t len)
{
	uint32_t free = ove_sim_ring_free_atomic(r);
	if (len > free) {
		len = free;
		r->overruns++;
	}
	if (len == 0)
		return 0;

	uint32_t mask = ove_sim_ring_mask(r);
	uint32_t wp = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
	const uint8_t *src = (const uint8_t *)data;

	for (uint32_t i = 0; i < len; i++)
		r->buf[(wp + i) & mask] = src[i];

	__atomic_store_n(&r->write_pos, wp + len, __ATOMIC_RELEASE);
	return len;
}

static inline uint32_t
ove_sim_ring_read_atomic(struct ove_sim_audio_ring *r,
			 void *data, uint32_t len)
{
	uint32_t wp = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
	uint32_t rp = __atomic_load_n(&r->read_pos, __ATOMIC_ACQUIRE);
	uint32_t avail = wp - rp;
	if (len > avail) {
		len = avail;
		if (avail == 0)
			r->underruns++;
	}
	if (len == 0)
		return 0;

	uint32_t mask = ove_sim_ring_mask(r);
	uint8_t *dst = (uint8_t *)data;

	for (uint32_t i = 0; i < len; i++)
		dst[i] = r->buf[(rp + i) & mask];

	__atomic_store_n(&r->read_pos, rp + len, __ATOMIC_RELEASE);
	return len;
}

#endif /* __EMSCRIPTEN__ */

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_AUDIO_RING_H */
