/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef STM32F7_INIT_H
#define STM32F7_INIT_H

/**
 * @brief STM32F7 MCU-level initialisation.
 *
 * Enables branch prediction, I-Cache, D-Cache (when safe), and calls
 * HAL_Init().  Must be called before board-specific clock configuration.
 */
void stm32f7_mcu_init(void);

#endif /* STM32F7_INIT_H */
