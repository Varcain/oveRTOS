/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifdef __EMSCRIPTEN__

#include "ove_sim_wasm_input.h"
#include <emscripten.h>

struct ove_sim_wasm_input ove_wasm_input;

EMSCRIPTEN_KEEPALIVE
void ove_wasm_input_set(int x, int y, int pressed)
{
	ove_wasm_input.x = (int16_t)x;
	ove_wasm_input.y = (int16_t)y;
	ove_wasm_input.pressed = (uint8_t)(pressed ? 1 : 0);
}

#endif /* __EMSCRIPTEN__ */
