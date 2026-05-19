/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file gpio.hpp
 * @brief GPIO pin configuration and control functions
 */

#pragma once

#include <ove/gpio.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_GPIO

namespace ove::gpio
{

/**
 * @namespace ove::gpio
 * @brief Thin C++ wrappers around the oveRTOS GPIO API.
 *
 * Available when `CONFIG_OVE_GPIO` is enabled.  Pins are addressed by a
 * (port, pin) tuple following the same convention as the underlying C API.
 */

/**
 * @brief Configures a GPIO pin with the specified mode.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @param[in] mode Desired pin mode.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> configure(unsigned int port, unsigned int pin,
					    ove_gpio_mode_t mode) noexcept
{
	return from_rc(ove_gpio_configure(port, pin, mode));
}

/**
 * @brief Drives a GPIO output pin to the specified logic level.
 * @param[in] port  GPIO port index.
 * @param[in] pin   Pin number within the port.
 * @param[in] value Logic level to drive (0 = low, non-zero = high).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> set(unsigned int port, unsigned int pin, int value) noexcept
{
	return from_rc(ove_gpio_set(port, pin, value));
}

/**
 * @brief Reads the current logic level of a GPIO pin.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @return On success, the pin level (0 or 1).  On failure, an
 *         `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<int> get(unsigned int port, unsigned int pin) noexcept
{
	const int rc = ove_gpio_get(port, pin);
	if (rc >= 0)
		return rc;
	return std::unexpected{static_cast<Error>(rc)};
}

/**
 * @brief Registers an interrupt callback for a GPIO pin.
 * @param[in] port      GPIO port index.
 * @param[in] pin       Pin number within the port.
 * @param[in] mode      Trigger mode (rising, falling, both edges, etc.).
 * @param[in] callback  Function to call when the interrupt fires.
 * @param[in] user_data Opaque pointer forwarded to the callback.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> irq_register(unsigned int port, unsigned int pin,
					       ove_gpio_irq_mode_t mode, ove_gpio_irq_cb callback,
					       void *user_data) noexcept
{
	return from_rc(ove_gpio_irq_register(port, pin, mode, callback, user_data));
}

// Undef RTOS macros that collide with our function names
#ifdef irq_enable
#undef irq_enable
#endif
#ifdef irq_disable
#undef irq_disable
#endif

/**
 * @brief Enables the interrupt for a GPIO pin (must be registered first).
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> irq_enable(unsigned int port, unsigned int pin) noexcept
{
	return from_rc(ove_gpio_irq_enable(port, pin));
}

/**
 * @brief Disables the interrupt for a GPIO pin.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> irq_disable(unsigned int port, unsigned int pin) noexcept
{
	return from_rc(ove_gpio_irq_disable(port, pin));
}

} /* namespace ove::gpio */

#endif /* CONFIG_OVE_GPIO */
