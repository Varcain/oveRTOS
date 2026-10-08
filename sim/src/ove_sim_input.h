/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Pointer input from the sim dashboard.
 *
 * The dashboard side keeps the pointer in /dev/shm/ove-input; the LVGL
 * input device driver reads it from there each tick.
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

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_INPUT_H */
