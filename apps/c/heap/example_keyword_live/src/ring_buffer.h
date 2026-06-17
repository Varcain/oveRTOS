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
 * No mutex needed: the head/tail indices are C11 atomics with
 * release/acquire ordering so the consumer that observes a new head is
 * guaranteed to also see the sample stored before it (a bare `volatile`
 * gives no such ordering, and `head++` across threads is a data race).
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* Must be power of 2 for masking. 32768 samples = 2 seconds at 16kHz. */
#define RING_BUF_CAPACITY 32768
#define RING_BUF_MASK (RING_BUF_CAPACITY - 1)

typedef struct {
	int16_t data[RING_BUF_CAPACITY];
	atomic_uint head; /* Next write position (audio callback) */
	atomic_uint tail; /* Next read position (inference thread) */
} ring_buffer_t;

static inline void ring_buffer_init(ring_buffer_t *rb)
{
	atomic_init(&rb->head, 0);
	atomic_init(&rb->tail, 0);
	memset(rb->data, 0, sizeof(rb->data));
}

/* Write samples from audio callback (single producer). */
static inline void ring_buffer_write(ring_buffer_t *rb, const int16_t *samples, unsigned int count)
{
	/* Only this (producer) thread writes head, so a relaxed load is enough. */
	unsigned int h = atomic_load_explicit(&rb->head, memory_order_relaxed);
	for (unsigned int i = 0; i < count; i++) {
		rb->data[h & RING_BUF_MASK] = samples[i];
		/* Release so a consumer that observes h+1 also sees the sample
		 * stored above. */
		atomic_store_explicit(&rb->head, h + 1, memory_order_release);
		h++;
	}
}

/* Number of samples available to read. */
static inline unsigned int ring_buffer_available(const ring_buffer_t *rb)
{
	/* Acquire on head pairs with the producer's release; tail is written
	 * only by this (consumer) thread, so a relaxed load suffices. */
	return atomic_load_explicit(&rb->head, memory_order_acquire) -
	       atomic_load_explicit(&rb->tail, memory_order_relaxed);
}

/*
 * Read the most recent N samples (for inference window).
 * Does NOT advance the tail — inference re-reads overlapping windows.
 */
static inline void ring_buffer_read_last(ring_buffer_t *rb, int16_t *out, unsigned int count)
{
	/* Acquire so the data stored before this head is visible below. */
	unsigned int h = atomic_load_explicit(&rb->head, memory_order_acquire);
	unsigned int start;

	if (h >= count)
		start = h - count;
	else
		start = 0;

	for (unsigned int i = 0; i < count; i++)
		out[i] = rb->data[(start + i) & RING_BUF_MASK];

	atomic_store_explicit(&rb->tail, h, memory_order_relaxed);
}

#endif /* RING_BUFFER_H */
