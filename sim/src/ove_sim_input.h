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
 * LVGL and Linux touch providers poll /dev/shm/ove-input.
 */

#ifndef OVE_SIM_INPUT_H
#define OVE_SIM_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return 1 if the one-shot open of the pointer file succeeded. */
int ove_sim_input_present(void);

/**
 * @brief Read the current pointer input state.
 */
void ove_sim_input_get(int16_t *x, int16_t *y, uint8_t *pressed);

#ifdef __cplusplus
}
#endif

#endif /* OVE_SIM_INPUT_H */
