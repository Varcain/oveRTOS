/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/hal/hal_gpio.h"
#include "ove_backend_common.h"
#include "stm32f7xx_hal.h"
#include "board_desc.h"

/* port_to_gpio() covers ports A-I of 16 pins: board.yaml's gpio must fit. */
_Static_assert(OVE_GPIO_PORT_COUNT <= 9 && OVE_GPIO_PINS_PER_PORT <= 16,
	       "board.yaml gpio exceeds the STM32 ports this backend maps");

static uint32_t gpio_irq_lock(void)
{
	uint32_t key = __get_PRIMASK();

	__disable_irq();
	return key;
}

static void gpio_irq_unlock(uint32_t key)
{
	__set_PRIMASK(key);
}

static GPIO_TypeDef *port_to_gpio(unsigned int port)
{
	switch (port) {
	case 0:
		return GPIOA;
	case 1:
		return GPIOB;
	case 2:
		return GPIOC;
	case 3:
		return GPIOD;
	case 4:
		return GPIOE;
	case 5:
		return GPIOF;
	case 6:
		return GPIOG;
	case 7:
		return GPIOH;
	case 8:
		return GPIOI;
	default:
		return NULL;
	}
}

static IRQn_Type pin_to_irqn(uint16_t pin)
{
	switch (pin) {
	case 0:
		return EXTI0_IRQn;
	case 1:
		return EXTI1_IRQn;
	case 2:
		return EXTI2_IRQn;
	case 3:
		return EXTI3_IRQn;
	case 4:
		return EXTI4_IRQn;
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
		return EXTI9_5_IRQn;
	default:
		return EXTI15_10_IRQn;
	}
}

int ove_hal_gpio_configure(unsigned int port, unsigned int pin, ove_gpio_mode_t mode)
{
	GPIO_TypeDef *gpio = port_to_gpio(port);
	GPIO_InitTypeDef gpio_init;

	if (gpio == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	gpio_init.Pin = (uint32_t)(1U << pin);
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;

	switch (mode) {
	case OVE_GPIO_MODE_INPUT:
		gpio_init.Mode = GPIO_MODE_INPUT;
		break;
	case OVE_GPIO_MODE_OUTPUT_PP:
		gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
		break;
	case OVE_GPIO_MODE_OUTPUT_OD:
		gpio_init.Mode = GPIO_MODE_OUTPUT_OD;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	HAL_GPIO_Init(gpio, &gpio_init);
	return OVE_OK;
}

int ove_hal_gpio_set(unsigned int port, unsigned int pin, int value)
{
	GPIO_TypeDef *gpio = port_to_gpio(port);
	if (gpio == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	HAL_GPIO_WritePin(gpio, (uint16_t)(1U << pin), value ? GPIO_PIN_SET : GPIO_PIN_RESET);
	return OVE_OK;
}

int ove_hal_gpio_get(unsigned int port, unsigned int pin)
{
	GPIO_TypeDef *gpio = port_to_gpio(port);
	if (gpio == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}
	return HAL_GPIO_ReadPin(gpio, (uint16_t)(1U << pin)) == GPIO_PIN_SET ? 1 : 0;
}

/*
 * EXTI line N serves pin N of every port, routed to one port at a time by
 * SYSCFG.  The hardware records the owner, so no table is kept: a line is
 * in use while it has an edge selected (HAL_GPIO_Init selects one for every
 * interrupt mode and HAL_GPIO_DeInit clears both, so this holds from
 * registration to unregistration, armed or not), and its owner is the port
 * it is routed to.
 */
static unsigned int exti_port(unsigned int pin)
{
	return (SYSCFG->EXTICR[pin >> 2] >> (4U * (pin & 3U))) & 0xFU;
}

/* OVE_ERR_BUSY while another port holds the line of @p pin. */
static int exti_check_owner(unsigned int port, unsigned int pin)
{
	if (((EXTI->RTSR | EXTI->FTSR) & (1U << pin)) != 0U && exti_port(pin) != port)
		return OVE_ERR_BUSY;
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_register(unsigned int port, unsigned int pin, ove_gpio_irq_mode_t mode)
{
	GPIO_TypeDef *gpio;
	GPIO_InitTypeDef gpio_init;
	uint32_t key;

	gpio = port_to_gpio(port);
	if (gpio == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	/* Configure GPIO as EXTI */
	gpio_init.Pin = (uint32_t)(1U << pin);
	gpio_init.Pull = GPIO_NOPULL;
	gpio_init.Speed = GPIO_SPEED_FREQ_LOW;

	switch (mode) {
	case OVE_GPIO_IRQ_RISING:
		gpio_init.Mode = GPIO_MODE_IT_RISING;
		break;
	case OVE_GPIO_IRQ_FALLING:
		gpio_init.Mode = GPIO_MODE_IT_FALLING;
		break;
	case OVE_GPIO_IRQ_BOTH:
		gpio_init.Mode = GPIO_MODE_IT_RISING_FALLING;
		break;
	default:
		return OVE_ERR_INVALID_PARAM;
	}

	key = gpio_irq_lock();
	if (exti_check_owner(port, pin) != OVE_OK) {
		gpio_irq_unlock(key);
		return OVE_ERR_BUSY;
	}
	HAL_GPIO_Init(gpio, &gpio_init);
	EXTI->IMR &= ~(1U << pin);
	EXTI->PR = 1U << pin; /* an edge latched for the line's previous owner */
	HAL_NVIC_SetPriority(pin_to_irqn(pin), 5, 0);
	HAL_NVIC_EnableIRQ(pin_to_irqn(pin));
	gpio_irq_unlock(key);
	return OVE_OK;
}

int ove_hal_gpio_irq_hw_enable(unsigned int port, unsigned int pin)
{
	if (port_to_gpio(port) == NULL || pin >= 16U)
		return OVE_ERR_INVALID_PARAM;

	uint32_t key = gpio_irq_lock();
	int ret = exti_check_owner(port, pin);
	if (ret == OVE_OK)
		EXTI->IMR |= 1U << pin;
	gpio_irq_unlock(key);
	return ret;
}

int ove_hal_gpio_irq_hw_disable(unsigned int port, unsigned int pin)
{
	if (port_to_gpio(port) == NULL || pin >= 16U)
		return OVE_ERR_INVALID_PARAM;

	/* EXTI5..9 and EXTI10..15 share NVIC vectors.  Mask only this line;
	 * disabling the vector would also silence unrelated registered pins. */
	uint32_t key = gpio_irq_lock();
	int ret = exti_check_owner(port, pin);
	if (ret == OVE_OK)
		EXTI->IMR &= ~(1U << pin);
	gpio_irq_unlock(key);
	return ret;
}

int ove_hal_gpio_irq_hw_unregister(unsigned int port, unsigned int pin)
{
	GPIO_TypeDef *gpio = port_to_gpio(port);
	uint32_t key;

	if (gpio == NULL || pin >= 16U)
		return OVE_ERR_INVALID_PARAM;

	/* HAL_GPIO_DeInit removes this line's EXTI route and trigger state while
	 * leaving the shared NVIC vector available to the other EXTI lines. */
	key = gpio_irq_lock();
	HAL_GPIO_DeInit(gpio, 1U << pin);
	gpio_irq_unlock(key);
	return OVE_OK;
}

/* HAL EXTI callback — dispatches to shared IRQ dispatch */
extern void ove_gpio_irq_dispatch(unsigned int port, unsigned int pin);

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	/* Reconstruct pin number from bitmask */
	unsigned int pin = 0;
	uint16_t tmp = GPIO_Pin;

	while (tmp > 1) {
		tmp >>= 1;
		pin++;
	}

	/* EXTI reports only the line; the port is the one it is routed to. */
	ove_gpio_irq_dispatch(exti_port(pin), pin);
}
