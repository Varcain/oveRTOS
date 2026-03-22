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

#ifdef CONFIG_OVE_GPIO

namespace ove {

/**
 * @namespace ove::gpio
 * @brief Thin C++ wrappers around the oveRTOS GPIO API.
 *
 * Available when `CONFIG_OVE_GPIO` is enabled.  Pins are addressed by a
 * (port, pin) tuple following the same convention as the underlying C API.
 */
namespace gpio {

/**
 * @brief Configures a GPIO pin with the specified mode.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @param[in] mode Desired pin mode (input, output, alternate function, etc.).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int configure(unsigned int port, unsigned int pin,
				    ove_gpio_mode_t mode) {
	return ove_gpio_configure(port, pin, mode);
}

/**
 * @brief Drives a GPIO output pin to the specified logic level.
 * @param[in] port  GPIO port index.
 * @param[in] pin   Pin number within the port.
 * @param[in] value Logic level to drive (0 = low, non-zero = high).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int set(unsigned int port, unsigned int pin,
			      int value) {
	return ove_gpio_set(port, pin, value);
}

/**
 * @brief Reads the current logic level of a GPIO pin.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @return 0 or 1 for the pin level, or a negative error code.
 */
[[nodiscard]] inline int get(unsigned int port, unsigned int pin) {
	return ove_gpio_get(port, pin);
}

/**
 * @brief Registers an interrupt callback for a GPIO pin.
 * @param[in] port      GPIO port index.
 * @param[in] pin       Pin number within the port.
 * @param[in] mode      Trigger mode (rising, falling, both edges, etc.).
 * @param[in] callback  Function to call when the interrupt fires.
 * @param[in] user_data Opaque pointer forwarded to the callback.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int irq_register(unsigned int port,
				       unsigned int pin,
				       ove_gpio_irq_mode_t mode,
				       ove_gpio_irq_cb callback,
				       void *user_data) {
	return ove_gpio_irq_register(port, pin, mode, callback,
					  user_data);
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
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int irq_enable(unsigned int port,
				     unsigned int pin) {
	return ove_gpio_irq_enable(port, pin);
}

/**
 * @brief Disables the interrupt for a GPIO pin.
 * @param[in] port GPIO port index.
 * @param[in] pin  Pin number within the port.
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int irq_disable(unsigned int port,
				      unsigned int pin) {
	return ove_gpio_irq_disable(port, pin);
}

} /* namespace gpio */

} // namespace ove

#endif /* CONFIG_OVE_GPIO */
