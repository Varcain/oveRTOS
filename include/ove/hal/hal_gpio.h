/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_HAL_GPIO_H
#define OVE_HAL_GPIO_H

/**
 * @defgroup ove_hal_gpio HAL GPIO Interface
 * @brief Hardware Abstraction Layer interface for GPIO operations.
 *
 * Declares the low-level GPIO functions that every platform HAL must
 * implement.  The portable @ref ove_gpio layer delegates to these
 * functions after performing parameter validation and IRQ bookkeeping.
 *
 * @note Platform implementations supply their own definitions of these
 *       functions in a board- or SoC-specific source file.
 * @{
 */

#include "ove/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set the output level of a GPIO pin at the hardware level.
 *
 * Called by ove_gpio_set() after validation.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @param[in] value Non-zero to drive high, zero to drive low.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value);

/**
 * @brief Read the current logical level of a GPIO pin at the hardware level.
 *
 * Called by ove_gpio_get() after validation.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return 1 if the pin is high, 0 if low, negative error code on failure.
 */
int ove_hal_gpio_get(unsigned int port, unsigned int pin);

/**
 * @brief Configure the direction and drive mode of a GPIO pin at the hardware level.
 *
 * Called by ove_gpio_configure() after validation.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @param[in] mode  Desired pin mode (@c ove_gpio_mode_t).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_gpio_configure(unsigned int port, unsigned int pin,
			       ove_gpio_mode_t mode);

/**
 * @brief Program and enable a GPIO interrupt in hardware.
 *
 * Called by ove_gpio_irq_enable() after the callback has been registered
 * via ove_gpio_irq_register().  The HAL must configure the edge detector
 * for @p mode and arm the interrupt controller.
 *
 * @param[in] port      GPIO port index.
 * @param[in] pin       GPIO pin index within the port.
 * @param[in] mode      Edge(s) that should trigger the interrupt.
 * @param[in] callback  Function to invoke when the interrupt fires.
 * @param[in] user_data Opaque pointer forwarded to @p callback.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin,
				   ove_gpio_irq_mode_t mode,
				   ove_gpio_irq_cb callback,
				   void *user_data);

/**
 * @brief Disable a GPIO interrupt in hardware without unregistering the callback.
 *
 * Called by ove_gpio_irq_disable().  The interrupt controller entry for
 * this pin must be masked so no further callbacks are dispatched.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_HAL_GPIO_H */
