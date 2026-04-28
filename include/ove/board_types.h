/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_BOARD_TYPES_H
#define OVE_BOARD_TYPES_H

/**
 * @defgroup ove_board_types Board Type Definitions
 * @brief Data structures used to describe a hardware board.
 *
 * These plain C structs are populated by each board's definition file
 * (typically in @c boards/\<name\>/) and consumed by the board and LED
 * subsystems.
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Descriptor for a single on-board LED.
 *
 * Identifies the GPIO pin that drives the LED and its polarity.
 */
struct ove_led_desc {
	unsigned int port; /**< GPIO port index of the LED pin. */
	unsigned int pin;  /**< GPIO pin index within the port. */
	int active_low;	   /**< Non-zero if the LED is lit when the pin is low. */
};

/**
 * @brief Full description of a hardware board.
 *
 * One instance of this struct is defined per supported board and
 * returned by ove_board_desc().  Fields may be zero/NULL when the
 * corresponding peripheral does not exist on the board.
 */
struct ove_board_desc {
	const char *name;	/**< Human-readable board name (e.g. @c "STM32F4-Discovery"). */
	const char *mcu_family; /**< MCU family string (e.g. @c "STM32F4"). */
	const char *mcu;	/**< Specific MCU part number (e.g. @c "STM32F407VGT6"). */
	unsigned int gpio_port_count;	 /**< Number of GPIO ports available on this board. */
	unsigned int gpio_pins_per_port; /**< Number of pins in each GPIO port. */
	unsigned int led_count;		 /**< Number of on-board LEDs described in @c leds. */
	const struct ove_led_desc *leds; /**< Array of LED descriptors, length @c led_count. */
};

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_BOARD_TYPES_H */
