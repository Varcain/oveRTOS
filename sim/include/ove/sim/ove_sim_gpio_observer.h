/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_gpio_observer Simulation GPIO Observer
 * @brief Observer and injection interface for simulated GPIO pins.
 *
 * Plugins (LEDs, buttons) register as observers to receive notifications
 * when GPIO pins change state.  The injection API allows the dashboard
 * to simulate external input (button presses, sensor interrupts).
 * @{
 */

#ifndef OVE_SIM_GPIO_OBSERVER_H
#define OVE_SIM_GPIO_OBSERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of concurrent GPIO observers. */
#define OVE_SIM_GPIO_MAX_OBSERVERS 32

/**
 * @brief GPIO state-change callback.
 *
 * @param[in] port       GPIO port index.
 * @param[in] pin        GPIO pin index within the port.
 * @param[in] new_value  New pin level (0 or 1).
 * @param[in] user_data  Opaque pointer from registration.
 */
typedef void (*ove_sim_gpio_cb)(unsigned int port, unsigned int pin, int new_value,
				void *user_data);

/**
 * @brief Register an observer for a specific GPIO pin.
 *
 * The callback fires whenever the pin's output level changes via
 * ove_hal_gpio_set() or ove_sim_gpio_inject().
 *
 * @param[in] port       GPIO port index.
 * @param[in] pin        GPIO pin index.
 * @param[in] cb         Callback function.
 * @param[in] user_data  Opaque pointer forwarded to @p cb.
 * @return 0 on success, negative error code if table full.
 */
int ove_sim_gpio_observe(unsigned int port, unsigned int pin, ove_sim_gpio_cb cb, void *user_data);

/**
 * @brief Remove an observer.
 *
 * @param[in] port       GPIO port index.
 * @param[in] pin        GPIO pin index.
 * @param[in] cb         Callback to remove.
 * @param[in] user_data  Matching user_data pointer.
 * @return 0 on success, negative error code if not found.
 */
int ove_sim_gpio_unobserve(unsigned int port, unsigned int pin, ove_sim_gpio_cb cb,
			   void *user_data);

/**
 * @brief Inject an external input value into a GPIO pin.
 *
 * Simulates an external signal (button press, sensor interrupt).
 * Updates the pin state and fires any registered IRQ callbacks
 * as well as observer callbacks.
 *
 * @param[in] port   GPIO port index.
 * @param[in] pin    GPIO pin index.
 * @param[in] value  New pin level (0 or 1).
 * @return 0 on success, negative error code on failure.
 */
int ove_sim_gpio_inject(unsigned int port, unsigned int pin, int value);

/**
 * @brief Notify observers of a GPIO state change.
 *
 * Called internally by sim_gpio.c when a pin changes.
 * Not intended for direct use by plugins.
 *
 * @param[in] port       GPIO port index.
 * @param[in] pin        GPIO pin index.
 * @param[in] new_value  New pin level (0 or 1).
 */
void ove_sim_gpio_notify(unsigned int port, unsigned int pin, int new_value);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_GPIO_OBSERVER_H */
