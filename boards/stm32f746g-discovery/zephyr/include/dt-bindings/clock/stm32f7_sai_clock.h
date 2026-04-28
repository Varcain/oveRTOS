/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */
#ifndef STM32F7_SAI_CLOCK_H_
#define STM32F7_SAI_CLOCK_H_

#include <zephyr/dt-bindings/clock/stm32f7_clock.h>

/* SAI clock source selection (DCKCFGR1 register)
 * These are not yet in upstream Zephyr's stm32f7_clock.h */
#define SAI1_SEL(val) STM32_DT_CLOCK_SELECT((val), 21, 20, DCKCFGR1_REG)
#define SAI2_SEL(val) STM32_DT_CLOCK_SELECT((val), 23, 22, DCKCFGR1_REG)

#endif /* STM32F7_SAI_CLOCK_H_ */
