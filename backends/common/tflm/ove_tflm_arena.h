/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef OVE_TFLM_ARENA_H
#define OVE_TFLM_ARENA_H

#include <cstddef>
#include <cstdint>

namespace tflite
{
class MicroAllocator;
}
class OveArenaAllocator;

struct OveTflmArena {
	OveArenaAllocator *buffer;
	tflite::MicroAllocator *allocator;
};

/* Conservative startup minimum, excluding original front alignment padding.
 * This only covers placement before the first checked SDK allocation. */
size_t ove_tflm_arena_minimum();

/* The caller supplies an aligned, cleared arena of at least the minimum. */
OveTflmArena ove_tflm_arena_create(uint8_t *arena, size_t bytes);
bool ove_tflm_arena_exhausted(const OveTflmArena &arena);

#endif
