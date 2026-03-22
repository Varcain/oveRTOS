/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef INC_STM32F7_BSP_H_
#define INC_STM32F7_BSP_H_

#include "stm32f7xx_hal.h"

int bsp_boardInit(void);
void bsp_toggleLed(unsigned int led);

#endif /* INC_STM32F429_BSP_H_ */
