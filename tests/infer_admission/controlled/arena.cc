// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tflm/ove_tflm_arena.h"
#include "tensorflow/lite/micro/micro_interpreter.h"

// This executable substitutes the complete SDK for admission-tail tests.
// Real arena boundaries and placement use the separate actual-library fixtures.
size_t ove_tflm_arena_minimum()
{
	return 64;
}

OveTflmArena ove_tflm_arena_create(uint8_t *arena, size_t bytes)
{
	return {nullptr, tflite::MicroAllocator::Create(arena, bytes)};
}

bool ove_tflm_arena_exhausted(const OveTflmArena &)
{
	return false;
}
