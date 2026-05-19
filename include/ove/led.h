/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LED_H
#define OVE_LED_H

/**
 * @file led.h
 * @defgroup ove_led LED
 * @brief Board LED control.
 *
 * Simple on/off and toggle control for the LEDs described by the active
 * board descriptor.  LED indices are 0-based and must be less than the
 * value returned by ove_led_count().
 *
 * @note Requires @c CONFIG_OVE_LED.  When the option is disabled every
 *       function is replaced by a no-op stub.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_LED

/**
 * @brief Turn a board LED on or off.
 *
 * Active-low polarity is handled transparently by the backend; callers
 * always pass a logical level.
 *
 * @param[in] led  Zero-based LED index (must be < ove_led_count()).
 * @param[in] on   Non-zero to turn the LED on, zero to turn it off.
 */
void ove_led_set(unsigned int led, int on);

/**
 * @brief Toggle the current state of a board LED.
 *
 * @param[in] led  Zero-based LED index (must be < ove_led_count()).
 */
void ove_led_toggle(unsigned int led);

/**
 * @brief Return the number of LEDs available on the current board.
 *
 * @return Number of LEDs, or 0 if none are defined.
 */
unsigned int ove_led_count(void);

#else /* !CONFIG_OVE_LED */

static inline void ove_led_set(unsigned int led, int on)
{
	(void)led;
	(void)on;
}
static inline void ove_led_toggle(unsigned int led)
{
	(void)led;
}
static inline unsigned int ove_led_count(void)
{
	return 0;
}

#endif /* CONFIG_OVE_LED */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LED_H */
