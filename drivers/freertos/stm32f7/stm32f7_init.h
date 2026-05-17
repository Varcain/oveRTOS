/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef STM32F7_INIT_H
#define STM32F7_INIT_H

#include "ove/types.h"
#include "stm32f7xx_hal.h"

/**
 * @brief STM32F7 MCU-level initialisation.
 *
 * Enables branch prediction, I-Cache, D-Cache (when safe), and calls
 * HAL_Init().  Must be called before board-specific clock configuration.
 */
void stm32f7_mcu_init(void);

/* Convert oveRTOS ns timeout to STM32 HAL ms timeout.
 * Fast path: ns < 4.29 s -> single-cycle UDIV on Cortex-M7. */
static inline uint32_t stm32f7_ns_to_hal_ms(uint64_t timeout_ns)
{
	if (timeout_ns == OVE_WAIT_FOREVER)
		return HAL_MAX_DELAY;
	uint64_t ms;
	if (timeout_ns <= (uint64_t)UINT32_MAX) {
		ms = (uint32_t)timeout_ns / 1000000u;
	} else {
		ms = timeout_ns / 1000000ULL;
	}
	if (ms >= (uint64_t)HAL_MAX_DELAY)
		ms = (uint64_t)HAL_MAX_DELAY - 1u;
	return (uint32_t)ms;
}

#endif /* STM32F7_INIT_H */
