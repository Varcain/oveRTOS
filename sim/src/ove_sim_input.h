/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared input state for the sim dashboard.
 *
 * Platform-agnostic: POSIX mode receives input via WebSocket frames,
 * WASM mode receives it via JS ccall.  Both write to the same global
 * state that the LVGL input device driver reads each tick.
 */

#ifndef OVE_SIM_INPUT_H
#define OVE_SIM_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the current pointer input state.
 */
void ove_sim_input_get(int16_t *x, int16_t *y, uint8_t *pressed);

/**
 * @brief Update the pointer input state.
 *
 * Called from the WS server thread (POSIX) or from JS via ccall (WASM).
 */
void ove_sim_input_set(int16_t x, int16_t y, uint8_t pressed);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_INPUT_H */
