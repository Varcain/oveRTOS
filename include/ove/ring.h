/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file ring.h
 * @brief Byte ring buffer shared by the stream backends.
 *
 * Each backend embeds an ove_ring in its ove_stream and supplies the locking
 * and blocking around it; these helpers only move bytes and keep the
 * indices. The caller must hold whatever lock guards the ring.
 */

#ifndef OVE_RING_H
#define OVE_RING_H

#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A ring of @c size bytes holding @c count of them, oldest at @c tail. */
struct ove_ring {
	unsigned char *buf;
	size_t size;
	size_t head; /**< Next byte written goes here. */
	size_t tail; /**< Next byte read comes from here. */
	size_t count;
};

/** @brief Attach @p buf of @p size bytes as an empty ring. */
static inline void ove_ring_setup(struct ove_ring *r, unsigned char *buf, size_t size)
{
	r->buf = buf;
	r->size = size;
	r->head = 0;
	r->tail = 0;
	r->count = 0;
}

/** @brief Drop every byte in the ring. */
static inline void ove_ring_clear(struct ove_ring *r)
{
	r->head = 0;
	r->tail = 0;
	r->count = 0;
}

/** @brief Copy up to @p len bytes in; returns how many fit. */
static inline size_t ove_ring_write(struct ove_ring *r, const unsigned char *src, size_t len)
{
	size_t space = r->size - r->count;
	size_t chunk = len < space ? len : space;
	size_t contig = r->size - r->head;

	if (contig < chunk) {
		memcpy(r->buf + r->head, src, contig);
		memcpy(r->buf, src + contig, chunk - contig);
	} else {
		memcpy(r->buf + r->head, src, chunk);
	}
	r->head = (r->head + chunk) % r->size;
	r->count += chunk;
	return chunk;
}

/** @brief Copy up to @p len bytes out; returns how many were there. */
static inline size_t ove_ring_read(struct ove_ring *r, unsigned char *dst, size_t len)
{
	size_t chunk = len < r->count ? len : r->count;
	size_t contig = r->size - r->tail;

	if (contig < chunk) {
		memcpy(dst, r->buf + r->tail, contig);
		memcpy(dst + contig, r->buf, chunk - contig);
	} else {
		memcpy(dst, r->buf + r->tail, chunk);
	}
	r->tail = (r->tail + chunk) % r->size;
	r->count -= chunk;
	return chunk;
}

#ifdef __cplusplus
}
#endif

#endif /* OVE_RING_H */
