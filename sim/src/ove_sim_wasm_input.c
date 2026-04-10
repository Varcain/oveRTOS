/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifdef __EMSCRIPTEN__

#include "ove_sim_wasm_input.h"
#include "ove_sim_input.h"
#include <emscripten.h>

struct ove_sim_wasm_input ove_wasm_input;

EMSCRIPTEN_KEEPALIVE
void ove_wasm_input_set(int x, int y, int pressed)
{
	int16_t sx = (int16_t)x;
	int16_t sy = (int16_t)y;
	uint8_t sp = (uint8_t)(pressed ? 1 : 0);

	ove_wasm_input.x = sx;
	ove_wasm_input.y = sy;
	ove_wasm_input.pressed = sp;

	/* Also update the shared input state used by sim_lvgl.c */
	ove_sim_input_set(sx, sy, sp);
}

#endif /* __EMSCRIPTEN__ */
