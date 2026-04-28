/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_GPIO_H
#define OVE_GPIO_H

/**
 * @defgroup ove_gpio GPIO
 * @brief General-purpose I/O pin control.
 *
 * Provides pin configuration, digital read/write, and interrupt
 * registration for GPIO ports and pins.
 *
 * @note Requires @c CONFIG_OVE_GPIO.  When the option is disabled every
 *       function is replaced by a no-op stub that returns
 *       @c OVE_ERR_NOT_SUPPORTED.
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPIO pin direction and drive mode.
 */
typedef enum {
	OVE_GPIO_MODE_INPUT = 0,     /**< High-impedance digital input. */
	OVE_GPIO_MODE_OUTPUT_PP = 1, /**< Push-pull digital output. */
	OVE_GPIO_MODE_OUTPUT_OD = 2, /**< Open-drain digital output. */
} ove_gpio_mode_t;

/**
 * @brief GPIO interrupt trigger edge selection.
 */
typedef enum {
	OVE_GPIO_IRQ_RISING = 0x01,  /**< Trigger on rising edge only. */
	OVE_GPIO_IRQ_FALLING = 0x02, /**< Trigger on falling edge only. */
	OVE_GPIO_IRQ_BOTH = 0x03,    /**< Trigger on both edges. */
} ove_gpio_irq_mode_t;

/**
 * @brief GPIO interrupt callback type.
 *
 * Called from interrupt context (or a deferred work item, depending on
 * the backend) when the configured edge is detected.
 *
 * @param[in] port      GPIO port index that generated the interrupt.
 * @param[in] pin       GPIO pin index that generated the interrupt.
 * @param[in] user_data Opaque pointer supplied at registration time.
 */
typedef void (*ove_gpio_irq_cb)(unsigned int port, unsigned int pin, void *user_data);

#ifdef CONFIG_OVE_GPIO

/**
 * @brief Configure the direction and drive mode of a GPIO pin.
 *
 * @param[in] port  GPIO port index (0-based).
 * @param[in] pin   GPIO pin index within the port (0-based).
 * @param[in] mode  Desired pin mode (@c ove_gpio_mode_t).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode);

/**
 * @brief Set the output level of a GPIO pin.
 *
 * The pin must have been configured as an output (@c OVE_GPIO_MODE_OUTPUT_PP
 * or @c OVE_GPIO_MODE_OUTPUT_OD) before calling this function.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @param[in] value Non-zero to drive the pin high, zero to drive it low.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_gpio_set(unsigned int port, unsigned int pin, int value);

/**
 * @brief Read the current logical level of a GPIO pin.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return 1 if the pin is high, 0 if low, negative error code on failure.
 */
int ove_gpio_get(unsigned int port, unsigned int pin);

/**
 * @brief Register an interrupt callback for a GPIO pin.
 *
 * The interrupt is registered but not enabled; call ove_gpio_irq_enable()
 * to arm it.
 *
 * @param[in] port      GPIO port index.
 * @param[in] pin       GPIO pin index within the port.
 * @param[in] mode      Edge(s) that should trigger the interrupt.
 * @param[in] callback  Function called when the interrupt fires.
 * @param[in] user_data Opaque pointer forwarded to @p callback.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_gpio_irq_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode,
			  ove_gpio_irq_cb callback, void *user_data);

/**
 * @brief Enable a previously registered GPIO interrupt.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_gpio_irq_enable(unsigned int port, unsigned int pin);

/**
 * @brief Disable a previously enabled GPIO interrupt without unregistering it.
 *
 * The callback and trigger edge are retained; call ove_gpio_irq_enable()
 * to re-arm.
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_gpio_irq_disable(unsigned int port, unsigned int pin);

#else /* !CONFIG_OVE_GPIO */

static inline int ove_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	(void)port;
	(void)pin;
	(void)mode;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	(void)value;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_gpio_get(unsigned int port, unsigned int pin)
{
	(void)port;
	(void)pin;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_gpio_irq_register(unsigned int port, unsigned int pin,
					ove_gpio_irq_mode_t mode, ove_gpio_irq_cb callback,
					void *user_data)
{
	(void)port;
	(void)pin;
	(void)mode;
	(void)callback;
	(void)user_data;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_gpio_irq_enable(unsigned int port, unsigned int pin)
{
	(void)port;
	(void)pin;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_gpio_irq_disable(unsigned int port, unsigned int pin)
{
	(void)port;
	(void)pin;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_GPIO */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_GPIO_H */
