/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Backward-compatibility shim — maps old ove_bsp_*() API to the new
 * split modules (board, gpio, led).  Existing application code continues
 * to compile unchanged.
 */

#ifndef OVE_BSP_H
#define OVE_BSP_H

/**
 * @defgroup ove_bsp BSP Compatibility Shim
 * @brief Backward-compatible Board Support Package API.
 *
 * This header maps the legacy @c ove_bsp_*() functions to the newer
 * split modules: @ref ove_board, @ref ove_gpio, and @ref ove_led.
 * New code should call the individual module APIs directly; this shim
 * exists purely for source-level backward compatibility.
 *
 * @note Requires @c CONFIG_OVE_BSP.  When the option is disabled every
 *       function is replaced by a no-op stub.
 * @{
 */

#include "ove_config.h"
#include "ove/board.h"
#include "ove/gpio.h"
#include "ove/led.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Re-export the IRQ types so callers that only included bsp.h still see them */
#ifndef OVE_GPIO_H
/**
 * @brief GPIO interrupt trigger edge selection (re-exported for BSP-only callers).
 *
 * @note Prefer including @c ove/gpio.h directly for new code.
 */
typedef enum {
	OVE_GPIO_IRQ_RISING  = 0x01, /**< Trigger on rising edge only. */
	OVE_GPIO_IRQ_FALLING = 0x02, /**< Trigger on falling edge only. */
	OVE_GPIO_IRQ_BOTH    = 0x03, /**< Trigger on both edges. */
} ove_gpio_irq_mode_t;

/**
 * @brief GPIO interrupt callback type (re-exported for BSP-only callers).
 *
 * @param[in] port      GPIO port index that generated the interrupt.
 * @param[in] pin       GPIO pin index that generated the interrupt.
 * @param[in] user_data Opaque pointer supplied at registration time.
 */
typedef void (*ove_gpio_irq_cb)(unsigned int port, unsigned int pin,
				    void *user_data);
#endif

#ifdef CONFIG_OVE_BSP

/**
 * @brief Initialise the board hardware (BSP compatibility wrapper).
 *
 * Delegates to ove_board_init().
 *
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_bsp_board_init(void)
{
	return ove_board_init();
}

/**
 * @brief Turn a board LED on or off (BSP compatibility wrapper).
 *
 * Delegates to ove_led_set().
 *
 * @param[in] led  Zero-based LED index.
 * @param[in] on   Non-zero to turn the LED on, zero to turn it off.
 */
static inline void ove_bsp_led_set(unsigned int led, int on)
{
	ove_led_set(led, on);
}

/**
 * @brief Toggle the current state of a board LED (BSP compatibility wrapper).
 *
 * Delegates to ove_led_toggle().
 *
 * @param[in] led  Zero-based LED index.
 */
static inline void ove_bsp_led_toggle(unsigned int led)
{
	ove_led_toggle(led);
}

/**
 * @brief Set the output level of a GPIO pin (BSP compatibility wrapper).
 *
 * Delegates to ove_gpio_set().
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @param[in] value Non-zero to drive high, zero to drive low.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_bsp_gpio_set(unsigned int port, unsigned int pin,
				       int value)
{
	return ove_gpio_set(port, pin, value);
}

/**
 * @brief Read the current level of a GPIO pin (BSP compatibility wrapper).
 *
 * Delegates to ove_gpio_get().
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return 1 if high, 0 if low, negative error code on failure.
 */
static inline int ove_bsp_gpio_get(unsigned int port, unsigned int pin)
{
	return ove_gpio_get(port, pin);
}

/**
 * @brief Register a GPIO interrupt callback (BSP compatibility wrapper).
 *
 * Delegates to ove_gpio_irq_register().
 *
 * @param[in] port      GPIO port index.
 * @param[in] pin       GPIO pin index within the port.
 * @param[in] mode      Edge(s) that trigger the interrupt.
 * @param[in] callback  Function called when the interrupt fires.
 * @param[in] user_data Opaque pointer forwarded to @p callback.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_bsp_gpio_irq_register(unsigned int port,
						 unsigned int pin,
						 ove_gpio_irq_mode_t mode,
						 ove_gpio_irq_cb callback,
						 void *user_data)
{
	return ove_gpio_irq_register(port, pin, mode, callback, user_data);
}

/**
 * @brief Enable a registered GPIO interrupt (BSP compatibility wrapper).
 *
 * Delegates to ove_gpio_irq_enable().
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_bsp_gpio_irq_enable(unsigned int port,
					       unsigned int pin)
{
	return ove_gpio_irq_enable(port, pin);
}

/**
 * @brief Disable a GPIO interrupt without unregistering it (BSP compatibility wrapper).
 *
 * Delegates to ove_gpio_irq_disable().
 *
 * @param[in] port  GPIO port index.
 * @param[in] pin   GPIO pin index within the port.
 * @return OVE_OK on success, negative error code on failure.
 */
static inline int ove_bsp_gpio_irq_disable(unsigned int port,
						unsigned int pin)
{
	return ove_gpio_irq_disable(port, pin);
}

#else /* !CONFIG_OVE_BSP */

static inline int ove_bsp_board_init(void) { return OVE_OK; }
static inline void ove_bsp_led_set(unsigned int led, int on) { (void)led; (void)on; }
static inline void ove_bsp_led_toggle(unsigned int led) { (void)led; }
static inline int ove_bsp_gpio_set(unsigned int port, unsigned int pin, int value) { (void)port; (void)pin; (void)value; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_bsp_gpio_get(unsigned int port, unsigned int pin) { (void)port; (void)pin; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_bsp_gpio_irq_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode, ove_gpio_irq_cb callback, void *user_data) { (void)port; (void)pin; (void)mode; (void)callback; (void)user_data; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_bsp_gpio_irq_enable(unsigned int port, unsigned int pin) { (void)port; (void)pin; return OVE_ERR_NOT_SUPPORTED; }
static inline int ove_bsp_gpio_irq_disable(unsigned int port, unsigned int pin) { (void)port; (void)pin; return OVE_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_OVE_BSP */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_BSP_H */
