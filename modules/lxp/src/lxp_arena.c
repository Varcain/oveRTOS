/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "lxp/lxp_config.h"

#if 1 /* arena is core to the lxp module */

#include "lxp/lxp_arena.h"

#include <string.h>

/*
 * First-fit free-list region allocator.
 *
 * The managed region is tiled by variable-size blocks linked in address
 * order. Each block carries a header; the payload follows, aligned to
 * LXP_ARENA_ALIGN. Allocation splits a free block when the remainder can
 * host another header plus a minimum slot; free coalesces with the
 * physically adjacent neighbours.
 */

#define ARENA_MAGIC 0x4f415242u /* "OARB" */

struct lxp_arena_blk {
	struct lxp_arena_blk *next; /* physically-next block, or NULL */
	struct lxp_arena_blk *prev; /* physically-prev block, or NULL */
	size_t payload;		    /* usable bytes after the header */
	uint32_t is_free;
	uint32_t magic;
};

/* Header size rounded up so the payload that follows stays aligned. */
#define ARENA_HDR \
	((sizeof(struct lxp_arena_blk) + (LXP_ARENA_ALIGN - 1)) & ~(size_t)(LXP_ARENA_ALIGN - 1))

static inline size_t arena_align(size_t n)
{
	return (n + (LXP_ARENA_ALIGN - 1)) & ~(size_t)(LXP_ARENA_ALIGN - 1);
}

static inline void *blk_payload(struct lxp_arena_blk *b)
{
	return (uint8_t *)b + ARENA_HDR;
}

static inline struct lxp_arena_blk *payload_blk(void *p)
{
	return (struct lxp_arena_blk *)((uint8_t *)p - ARENA_HDR);
}

static void arena_lay_first(lxp_arena_t *arena)
{
	struct lxp_arena_blk *first = (struct lxp_arena_blk *)arena->base;
	first->next = NULL;
	first->prev = NULL;
	first->payload = (arena->size - ARENA_HDR) & ~(size_t)(LXP_ARENA_ALIGN - 1);
	first->is_free = 1;
	first->magic = ARENA_MAGIC;
	arena->first = first;
	arena->used = 0;
}

int lxp_arena_init(lxp_arena_t *arena, void *buf, size_t size)
{
	if (!arena || !buf)
		return LXP_ERR_INVALID_PARAM;

	uintptr_t raw = (uintptr_t)buf;
	uintptr_t start = (raw + (LXP_ARENA_ALIGN - 1)) & ~(uintptr_t)(LXP_ARENA_ALIGN - 1);
	size_t adjust = (size_t)(start - raw);
	if (size <= adjust)
		return LXP_ERR_INVALID_PARAM;

	size_t usable = size - adjust;
	if (usable < ARENA_HDR + LXP_ARENA_ALIGN)
		return LXP_ERR_NO_MEMORY;

	arena->base = (uint8_t *)buf + adjust; /* == aligned `start`, via ptr arithmetic */
	arena->size = usable;
	arena->high_water = 0;
	arena_lay_first(arena);
	return LXP_OK;
}

void *lxp_arena_alloc(lxp_arena_t *arena, size_t size)
{
	if (!arena || !arena->first)
		return NULL;

	size_t need = arena_align(size == 0 ? 1 : size);

	struct lxp_arena_blk *b = (struct lxp_arena_blk *)arena->first;
	while (b && !(b->is_free && b->payload >= need))
		b = b->next;
	if (!b)
		return NULL;

	/* Split when the tail can host another header plus a minimum slot. */
	if (b->payload >= need + ARENA_HDR + LXP_ARENA_ALIGN) {
		struct lxp_arena_blk *rem =
			(struct lxp_arena_blk *)((uint8_t *)b + ARENA_HDR + need);
		rem->payload = b->payload - need - ARENA_HDR;
		rem->is_free = 1;
		rem->magic = ARENA_MAGIC;
		rem->prev = b;
		rem->next = b->next;
		if (rem->next)
			rem->next->prev = rem;
		b->next = rem;
		b->payload = need;
	}

	b->is_free = 0;
	arena->used += ARENA_HDR + b->payload;
	if (arena->used > arena->high_water)
		arena->high_water = arena->used;
	return blk_payload(b);
}

void *lxp_arena_calloc(lxp_arena_t *arena, size_t size)
{
	void *p = lxp_arena_alloc(arena, size);
	if (p) {
		struct lxp_arena_blk *b = payload_blk(p);
		memset(p, 0, b->payload);
	}
	return p;
}

void lxp_arena_free(lxp_arena_t *arena, void *ptr)
{
	if (!arena || !ptr || !lxp_arena_owns(arena, ptr))
		return;

	struct lxp_arena_blk *b = payload_blk(ptr);
	if (b->magic != ARENA_MAGIC || b->is_free)
		return; /* corruption or double free — ignore defensively */

	b->is_free = 1;
	arena->used -= ARENA_HDR + b->payload;

	/* Coalesce forward. */
	struct lxp_arena_blk *n = b->next;
	if (n && n->is_free) {
		b->payload += ARENA_HDR + n->payload;
		b->next = n->next;
		if (n->next)
			n->next->prev = b;
		n->magic = 0;
	}

	/* Coalesce backward. */
	struct lxp_arena_blk *p = b->prev;
	if (p && p->is_free) {
		p->payload += ARENA_HDR + b->payload;
		p->next = b->next;
		if (b->next)
			b->next->prev = p;
		b->magic = 0;
	}
}

void lxp_arena_reset(lxp_arena_t *arena)
{
	if (!arena || !arena->base)
		return;
	arena_lay_first(arena);
}

bool lxp_arena_owns(const lxp_arena_t *arena, const void *ptr)
{
	if (!arena || !ptr)
		return false;
	const uint8_t *p = (const uint8_t *)ptr;
	return p >= arena->base && p < arena->base + arena->size;
}

size_t lxp_arena_used(const lxp_arena_t *arena)
{
	return arena ? arena->used : 0;
}

size_t lxp_arena_capacity(const lxp_arena_t *arena)
{
	return arena ? arena->size : 0;
}

size_t lxp_arena_high_water(const lxp_arena_t *arena)
{
	return arena ? arena->high_water : 0;
}

#endif /* CONFIG_LXP_ARENA */
