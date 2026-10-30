/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * ove GPIO HAL for the STM32F746G-DISCO on NuttX, on NuttX's own STM32F7 GPIO
 * and EXTI driver, so pin interrupts share the EXTI lines with NuttX's drivers
 * instead of fighting them over the registers.
 */

#include <nuttx/config.h>
#include <nuttx/irq.h>
#include <stdbool.h>
#include <stdint.h>

#include "ove/hal/hal_gpio.h"
#include "ove/types.h"
#include "board_desc.h"

/* arch/arm/src/stm32f7/stm32_gpio.h is arch-private (not on the app include
 * path); this is a BUILD_FLAT link, so declare what is used and mirror the pin
 * encoding it defines. */
extern int stm32_configgpio(uint32_t cfgset);
extern void stm32_gpiowrite(uint32_t pinset, bool value);
extern bool stm32_gpioread(uint32_t pinset);
extern int stm32_gpiosetevent(uint32_t pinset, bool risingedge, bool fallingedge, bool event,
			      xcpt_t func, void *arg);

#define PIN_INPUT (0u << 18)		       /* GPIO_INPUT */
#define PIN_OUTPUT (1u << 18)		       /* GPIO_OUTPUT */
#define PIN_FLOAT (0u << 16)		       /* GPIO_FLOAT */
#define PIN_SPEED_LOW (0u << 10)	       /* GPIO_SPEED_2MHz */
#define PIN_OPENDRAIN (1u << 9)		       /* GPIO_OPENDRAIN */
#define PIN_PUSHPULL 0u			       /* GPIO_PUSHPULL */
#define PIN_OUTPUT_CLEAR 0u		       /* GPIO_OUTPUT_CLEAR */
#define PIN_PORT(port) ((uint32_t)(port) << 4) /* GPIO_PORT_SHIFT, GPIO_PORTA = 0 */
#define PIN_NUM(pin) ((uint32_t)(pin) << 0)    /* GPIO_PIN_SHIFT */
#define PIN_EXTI (1u << 8)		       /* GPIO_EXTI: route the EXTI line here */

#define EXTI_PR (*(volatile uint32_t *)0x40013C14u)    /* STM32_EXTI_PR */
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280u) /* NVIC_IRQ0_31_CLRPEND */
#define NVIC_EXTI0 6U				       /* STM32_IRQ_EXTI0 - STM32_IRQ_FIRST */

/* NuttX's STM32F7 GPIO driver covers ports A-K of 16 pins. */
_Static_assert(OVE_GPIO_PORT_COUNT <= 11 && OVE_GPIO_PINS_PER_PORT <= 16,
	       "board.yaml gpio exceeds the STM32F7 ports NuttX drives");

static ove_gpio_irq_mode_t g_irq_mode[OVE_GPIO_PORT_COUNT][OVE_GPIO_PINS_PER_PORT];

static int pin_valid(unsigned int port, unsigned int pin)
{
	return port < OVE_GPIO_PORT_COUNT && pin < OVE_GPIO_PINS_PER_PORT;
}

static uint32_t pinset(unsigned int port, unsigned int pin)
{
	return PIN_PORT(port) | PIN_NUM(pin);
}

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	uint32_t cfg = pinset(port, pin) | PIN_FLOAT | PIN_SPEED_LOW;

	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;

	switch (mode) {
	case OVE_GPIO_MODE_INPUT:
		cfg |= PIN_INPUT;
		break;
	case OVE_GPIO_MODE_OUTPUT_PP:
		cfg |= PIN_OUTPUT | PIN_PUSHPULL | PIN_OUTPUT_CLEAR;
		break;
	case OVE_GPIO_MODE_OUTPUT_OD:
		cfg |= PIN_OUTPUT | PIN_OPENDRAIN | PIN_OUTPUT_CLEAR;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}
	return stm32_configgpio(cfg) == 0 ? OVE_OK : OVE_ERR_INVALID_PARAM;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;
	stm32_gpiowrite(pinset(port, pin), value != 0);
	return OVE_OK;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;
	return stm32_gpioread(pinset(port, pin)) ? 1 : 0;
}

extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);

static int gpio_isr(int irq, void *context, void *arg)
{
	uintptr_t line = (uintptr_t)arg;

	(void)irq;
	(void)context;
	ove_gpio_irq_dispatch((unsigned int)(line >> 4), (unsigned int)(line & 0xfu));
	return 0;
}

/* Configure the pin as an input with the registered edges; with a handler the
 * EXTI line is unmasked, without one NuttX leaves it masked. */
static int irq_config(unsigned int port, unsigned int pin, xcpt_t handler)
{
	ove_gpio_irq_mode_t mode = g_irq_mode[port][pin];

	return stm32_gpiosetevent(pinset(port, pin) | PIN_INPUT | PIN_FLOAT,
				  (mode & OVE_GPIO_IRQ_RISING) != 0,
				  (mode & OVE_GPIO_IRQ_FALLING) != 0, false, handler,
				  handler ? (void *)(uintptr_t)((port << 4) | pin) : NULL) == 0
		       ? OVE_OK
		       : OVE_ERR_INVALID_PARAM;
}

/* EXTI line N serves pin N of every port, one port at a time, and NuttX's
 * EXTI driver keeps one handler per line: another port's pin would replace
 * the owner's handler and route.  A line belongs to the port whose pin has
 * a registered mode, from registration to unregistration, armed or not.
 * Interrupts set up by NuttX's own drivers are not seen here. */
static int line_check_owner(unsigned int port, unsigned int pin)
{
	for (unsigned int p = 0; p < OVE_GPIO_PORT_COUNT; p++) {
		if (p != port && g_irq_mode[p][pin] != 0)
			return OVE_ERR_BUSY;
	}
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;
	if (mode != OVE_GPIO_IRQ_RISING && mode != OVE_GPIO_IRQ_FALLING &&
	    mode != OVE_GPIO_IRQ_BOTH)
		return OVE_ERR_INVALID_PARAM;

	irqstate_t flags = enter_critical_section();
	int ret = line_check_owner(port, pin);
	if (ret == OVE_OK) {
		g_irq_mode[port][pin] = mode;
		ret = irq_config(port, pin, NULL);
		if (ret != OVE_OK) {
			g_irq_mode[port][pin] = 0;
		} else {
			/* The masked line may still be routed to the port that held
			 * it before, and may hold an edge latched for it: route it
			 * here, then drop that edge, so it cannot reach this pin's
			 * callback once armed.  Lines 0-4 have vectors of their own,
			 * whose NuttX handlers run the callback without looking at
			 * the pending register, so their vector's pending state goes
			 * too; the shared vectors of lines 5-15 check it. */
			(void)stm32_configgpio(pinset(port, pin) | PIN_INPUT | PIN_FLOAT |
					       PIN_EXTI);
			EXTI_PR = 1U << pin;
			if (pin < 5U) {
				/* Let the clear land first, or the line could pend the
				 * vector again after it is cleared. */
				(void)EXTI_PR;
				__asm__ volatile("dsb" ::: "memory");
				NVIC_ICPR0 = 1U << (NVIC_EXTI0 + pin);
			}
		}
	}
	leave_critical_section(flags);
	return ret;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;

	irqstate_t flags = enter_critical_section();
	int ret = line_check_owner(port, pin);
	if (ret == OVE_OK)
		ret = g_irq_mode[port][pin] != 0 ? irq_config(port, pin, gpio_isr)
						 : OVE_ERR_INVALID_PARAM;
	leave_critical_section(flags);
	return ret;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;

	irqstate_t flags = enter_critical_section();
	int ret = line_check_owner(port, pin);
	if (ret == OVE_OK)
		ret = irq_config(port, pin, NULL);
	leave_critical_section(flags);
	return ret;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	if (!pin_valid(port, pin))
		return OVE_ERR_INVALID_PARAM;

	irqstate_t flags = enter_critical_section();
	int ret = line_check_owner(port, pin);
	if (ret == OVE_OK) {
		ret = irq_config(port, pin, NULL);
		if (ret == OVE_OK)
			g_irq_mode[port][pin] = 0;
	}
	leave_critical_section(flags);
	return ret;
}
