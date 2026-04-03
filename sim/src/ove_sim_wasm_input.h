/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared input state for WASM mode.
 *
 * JS writes mouse/touch coordinates into this struct via an exported
 * C function.  The LVGL input device driver reads from it each tick.
 */

#ifndef OVE_SIM_WASM_INPUT_H
#define OVE_SIM_WASM_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ove_sim_wasm_input {
	volatile int16_t x;
	volatile int16_t y;
	volatile uint8_t pressed; /* 1 = pressed, 0 = released */
};

extern struct ove_sim_wasm_input ove_wasm_input;

void ove_wasm_input_set(int x, int y, int pressed);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_WASM_INPUT_H */
