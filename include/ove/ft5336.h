/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_FT5336_H
#define OVE_FT5336_H

/**
 * @defgroup ove_ft5336 FT5336 capacitive touch
 * @brief Minimal driver for the FT5336 controller on the STM32F746-Discovery.
 *
 * A pure @ref ove_i2c client (address 0x38). The Linux personality's evdev class
 * polls @ref ove_ft5336_read from the run-loop tick and turns the result into
 * /dev/input/event0 events. Coordinate orientation matches the LTDC panel.
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bring up the touch controller (i2c bus + probe). Returns 0, or <0. */
int ove_ft5336_init(void);

/**
 * @brief Read the primary touch point.
 * @param[out] x        X coordinate (panel pixels) when pressed.
 * @param[out] y        Y coordinate (panel pixels) when pressed.
 * @param[out] pressed  1 if a finger is down, 0 otherwise.
 * @return 0 on a successful read (pressed reflects contact), <0 on an i2c error.
 */
int ove_ft5336_read(int *x, int *y, int *pressed);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_FT5336_H */
