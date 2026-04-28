/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file led.hpp
 * @brief On-board LED control functions
 */

#pragma once

#include <ove/led.h>
#include <ove/types.hpp>

#ifdef CONFIG_OVE_LED

namespace ove
{

/**
 * @namespace ove::led
 * @brief Thin C++ wrappers around the oveRTOS LED control API.
 *
 * Available when `CONFIG_OVE_LED` is enabled.  LEDs are identified by a
 * zero-based index.
 */
namespace led
{

/**
 * @brief Turns a LED on or off.
 * @param[in] led LED index (0-based).
 * @param[in] on  Non-zero to turn on, zero to turn off.
 */
inline void set(unsigned int led, int on)
{
	ove_led_set(led, on);
}

/**
 * @brief Toggles the state of a LED.
 * @param[in] led LED index (0-based).
 */
inline void toggle(unsigned int led)
{
	ove_led_toggle(led);
}

/**
 * @brief Returns the total number of LEDs available on this board.
 * @return Number of LEDs.
 */
inline unsigned int count()
{
	return ove_led_count();
}

} /* namespace led */

} // namespace ove

#endif /* CONFIG_OVE_LED */
