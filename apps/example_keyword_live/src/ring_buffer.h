/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Lock-free single-producer single-consumer ring buffer for PCM audio.
 *
 * The audio ISR/callback writes samples, the inference thread reads.
 * No mutex needed — volatile head/tail with power-of-2 masking.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <string.h>

/* Must be power of 2 for masking. 32768 samples = 2 seconds at 16kHz. */
#define RING_BUF_CAPACITY 32768
#define RING_BUF_MASK     (RING_BUF_CAPACITY - 1)

typedef struct {
	int16_t data[RING_BUF_CAPACITY];
	volatile unsigned int head;  /* Next write position (audio callback) */
	volatile unsigned int tail;  /* Next read position (inference thread) */
} ring_buffer_t;

static inline void ring_buffer_init(ring_buffer_t *rb)
{
	rb->head = 0;
	rb->tail = 0;
	memset(rb->data, 0, sizeof(rb->data));
}

/* Write samples from audio callback (single producer). */
static inline void ring_buffer_write(ring_buffer_t *rb,
				     const int16_t *samples,
				     unsigned int count)
{
	for (unsigned int i = 0; i < count; i++) {
		rb->data[rb->head & RING_BUF_MASK] = samples[i];
		rb->head++;
	}
}

/* Number of samples available to read. */
static inline unsigned int ring_buffer_available(const ring_buffer_t *rb)
{
	return rb->head - rb->tail;
}

/*
 * Read the most recent N samples (for inference window).
 * Does NOT advance the tail — inference re-reads overlapping windows.
 */
static inline void ring_buffer_read_last(ring_buffer_t *rb,
					 int16_t *out,
					 unsigned int count)
{
	unsigned int h = rb->head;
	unsigned int start;

	if (h >= count)
		start = h - count;
	else
		start = 0;

	for (unsigned int i = 0; i < count; i++)
		out[i] = rb->data[(start + i) & RING_BUF_MASK];

	rb->tail = h;
}

#endif /* RING_BUFFER_H */
