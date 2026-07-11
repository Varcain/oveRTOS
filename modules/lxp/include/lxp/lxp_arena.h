/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef LXP_ARENA_H
#define LXP_ARENA_H

/**
 * @file arena.h
 * @defgroup lxp_arena Arena Allocator
 * @ingroup ove_mem
 * @brief Bounded, backend-independent region allocator.
 *
 * An arena manages a single caller-supplied, fixed-size buffer and hands out
 * aligned blocks from it via a first-fit free list with boundary coalescing.
 * The total footprint is decided at build time (the buffer the caller passes
 * to @c lxp_arena_init), so an arena never grows the system heap — making it
 * suitable for zero-heap deployments and for carving a private, fault-bounded
 * pool for a subsystem.
 *
 * The arena is deliberately decoupled from any RTOS backend: it touches no
 * @c ove_* primitive and performs no locking. Callers that share an arena
 * across threads must provide their own mutual exclusion (e.g. an
 * @c ove_mutex around the alloc/free calls).
 *
 * All blocks are aligned to @c LXP_ARENA_ALIGN. Returned pointers may be
 * released in any order; adjacent free blocks coalesce so a fully-freed
 * arena returns to a single contiguous extent.
 *
 * @note Requires @c CONFIG_LXP_ARENA.
 * @{
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "lxp/lxp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Alignment (bytes) of every block handed out by the arena. */
#define LXP_ARENA_ALIGN 16u

/**
 * @brief Arena control block.
 *
 * Allocate one per managed buffer. The fields are exposed so the control
 * block can live in static storage (zero-heap), but they are an
 * implementation detail — use the accessor functions rather than reading
 * them directly.
 */
typedef struct lxp_arena {
	uint8_t *base;	   /**< Aligned start of the managed region. */
	size_t size;	   /**< Usable bytes of the managed region. */
	size_t used;	   /**< Footprint (header + payload) currently allocated. */
	size_t high_water; /**< Peak @c used observed since init. */
	void *first;	   /**< Head of the address-ordered block list. */
} lxp_arena_t;

/**
 * @brief Initialise an arena over a caller-supplied buffer.
 *
 * The buffer is consumed in full as the arena's backing store; its alignment
 * need not match @c LXP_ARENA_ALIGN (the arena aligns internally, possibly
 * skipping a few leading bytes).
 *
 * @param[out] arena Control block to initialise.
 * @param[in]  buf   Backing buffer.
 * @param[in]  size  Size of @p buf in bytes.
 * @return LXP_OK on success, LXP_ERR_INVALID_PARAM on NULL args,
 *         LXP_ERR_NO_MEMORY if the buffer is too small to host one block.
 * @note Requires @c CONFIG_LXP_ARENA.
 */
int lxp_arena_init(lxp_arena_t *arena, void *buf, size_t size);

/**
 * @brief Allocate an @c LXP_ARENA_ALIGN-aligned block.
 * @param[in] arena Initialised arena.
 * @param[in] size  Requested bytes (0 is treated as 1).
 * @return Pointer to the block, or NULL if no free extent is large enough.
 */
void *lxp_arena_alloc(lxp_arena_t *arena, size_t size);

/**
 * @brief Allocate a zero-filled block (see @c lxp_arena_alloc).
 */
void *lxp_arena_calloc(lxp_arena_t *arena, size_t size);

/**
 * @brief Release a block previously returned by this arena.
 *
 * NULL and pointers outside the arena are ignored. Double frees and
 * corrupted headers are ignored defensively rather than aborting.
 */
void lxp_arena_free(lxp_arena_t *arena, void *ptr);

/**
 * @brief Release every block, returning the arena to a single free extent.
 *
 * Outstanding pointers become invalid. @c high_water is preserved.
 */
void lxp_arena_reset(lxp_arena_t *arena);

/** @brief Non-zero if @p ptr lies within the arena's managed region. */
bool lxp_arena_owns(const lxp_arena_t *arena, const void *ptr);

/** @brief Footprint (header + payload) currently allocated, in bytes. */
size_t lxp_arena_used(const lxp_arena_t *arena);

/** @brief Total managed bytes (fixed at init). */
size_t lxp_arena_capacity(const lxp_arena_t *arena);

/** @brief Peak @c lxp_arena_used observed since init. */
size_t lxp_arena_high_water(const lxp_arena_t *arena);

/**
 * @brief Declare a static buffer + control block for an arena.
 *
 * Pairs with @c lxp_arena_init(&name, name##_storage, sizeof(name##_storage)).
 */
#define LXP_ARENA_DEFINE_STATIC(name, bytes)                                              \
	static uint8_t name##_storage[(bytes)] __attribute__((aligned(LXP_ARENA_ALIGN))); \
	static lxp_arena_t name

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* LXP_ARENA_H */
