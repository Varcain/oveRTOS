/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file bsp.hpp
 * @brief Legacy BSP compatibility shim
 */

#pragma once

#include <ove/bsp.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_BSP

namespace ove::bsp
{

/**
 * @namespace ove::bsp
 * @brief Backward-compatibility wrappers delegating to `board`, `gpio`, and `led`.
 *
 * Available when `CONFIG_OVE_BSP` is enabled.  New code should prefer the
 * individual `ove::board`, `ove::gpio`, and `ove::led` namespaces.
 */

/**
 * @brief Initialises the board hardware (backward-compatibility alias for `board::init`).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> board_init() noexcept
{
	return from_rc(ove_bsp_board_init());
}

/**
 * @brief Turns a LED on or off (backward-compatibility alias for `led::set`).
 * @param[in] led LED index (0-based).
 * @param[in] on  Non-zero to turn on, zero to turn off.
 */
inline void led_set(unsigned int led, int on)
{
	ove_bsp_led_set(led, on);
}

/**
 * @brief Toggles a LED (backward-compatibility alias for `led::toggle`).
 * @param[in] led LED index (0-based).
 */
inline void led_toggle(unsigned int led)
{
	ove_bsp_led_toggle(led);
}

/**
 * @brief Drives a GPIO output pin (backward-compatibility alias for `gpio::set`).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> gpio_set(unsigned int port, unsigned int pin, int value) noexcept
{
	return from_rc(ove_bsp_gpio_set(port, pin, value));
}

/**
 * @brief Reads a GPIO pin level (backward-compatibility alias for `gpio::get`).
 * @return On success, the pin level (0 or 1).  On failure, an
 *         `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<int> gpio_get(unsigned int port, unsigned int pin) noexcept
{
	const int rc = ove_bsp_gpio_get(port, pin);
	if (rc >= 0)
		return rc;
	return std::unexpected{static_cast<Error>(rc)};
}

/**
 * @brief Registers a GPIO interrupt callback (backward-compatibility alias for `gpio::irq_register`).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> gpio_irq_register(unsigned int port, unsigned int pin,
						    ove_gpio_irq_mode_t mode,
						    ove_gpio_irq_cb callback,
						    void *user_data) noexcept
{
	return from_rc(ove_bsp_gpio_irq_register(port, pin, mode, callback, user_data));
}

/**
 * @brief Enables a GPIO interrupt (backward-compatibility alias for `gpio::irq_enable`).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> gpio_irq_enable(unsigned int port, unsigned int pin) noexcept
{
	return from_rc(ove_bsp_gpio_irq_enable(port, pin));
}

/**
 * @brief Disables a GPIO interrupt (backward-compatibility alias for `gpio::irq_disable`).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> gpio_irq_disable(unsigned int port, unsigned int pin) noexcept
{
	return from_rc(ove_bsp_gpio_irq_disable(port, pin));
}

} /* namespace ove::bsp */

#endif /* CONFIG_OVE_BSP */
