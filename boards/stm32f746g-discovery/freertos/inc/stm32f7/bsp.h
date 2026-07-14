/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef INC_STM32F7_BSP_H_
#define INC_STM32F7_BSP_H_

#include <stddef.h>

#include "stm32f7xx_hal.h"

int bsp_boardInit(void);
void bsp_toggleLed(unsigned int led);

/* Board entropy source. Init is non-fatal to the host: bsp_random_fill returns
 * an error until the STM32 RNG is healthy. Fill is task-context-only,
 * all-or-error, and bounded to 4096 bytes / one finite total deadline. */
int bsp_random_init(void);
int bsp_random_fill(void *buf, size_t len);

/* Add an FMC read-pipe delay cycle for reliable SDRAM reads at 108 MHz under LTDC
 * contention. Must be re-applied after anything that re-runs BSP_SDRAM_Init —
 * notably BSP_LCD_Init, which does so internally. See bsp.c for the rationale. */
void bsp_sdram_fixup(void);

#endif /* INC_STM32F429_BSP_H_ */
