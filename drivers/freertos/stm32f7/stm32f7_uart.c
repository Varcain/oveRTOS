/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_UART

#include "ove/hal/hal_uart.h"
#include "ove_backend_common.h"
#include "stm32f7xx_hal.h"
#include <string.h>

/* Forward declaration of the portable ISR push helper */
extern void ove_uart_rx_isr_push(ove_uart_t uart, const void *data, size_t len);

/* Static table mapping instance → handle for ISR dispatch */
#define UART_MAX_INSTANCES 4
static ove_uart_t uart_instances[UART_MAX_INSTANCES];

static USART_TypeDef *instance_to_periph(unsigned int instance)
{
	switch (instance) {
	case 0:
		return USART1;
	case 1:
		return USART2;
	case 2:
		return USART3;
#ifdef UART4
	case 3:
		return UART4;
#endif
	default:
		return NULL;
	}
}

static IRQn_Type instance_to_irqn(unsigned int instance)
{
	switch (instance) {
	case 0:
		return USART1_IRQn;
	case 1:
		return USART2_IRQn;
	case 2:
		return USART3_IRQn;
#ifdef UART4_IRQn
	case 3:
		return UART4_IRQn;
#endif
	default:
		return USART1_IRQn;
	}
}

int ove_hal_uart_open(ove_uart_t uart, const struct ove_uart_cfg *cfg)
{
	USART_TypeDef *periph = instance_to_periph(cfg->instance);

	if (periph == NULL || cfg->instance >= UART_MAX_INSTANCES)
		return OVE_ERR_INVALID_PARAM;

	memset(&uart->hal_handle, 0, sizeof(uart->hal_handle));
	uart->hal_handle.Instance = periph;
	uart->hal_handle.Init.BaudRate = cfg->baudrate;
	uart->hal_handle.Init.StopBits = (cfg->stop_bits == OVE_UART_STOP_2) ? UART_STOPBITS_2
									     : UART_STOPBITS_1;
	uart->hal_handle.Init.HwFlowCtl = (cfg->flow_control == OVE_UART_FLOW_RTS_CTS)
						  ? UART_HWCONTROL_RTS_CTS
						  : UART_HWCONTROL_NONE;
	uart->hal_handle.Init.Mode = UART_MODE_TX_RX;
	uart->hal_handle.Init.OverSampling = UART_OVERSAMPLING_16;

	switch (cfg->parity) {
	case OVE_UART_PARITY_ODD:
		uart->hal_handle.Init.Parity = UART_PARITY_ODD;
		uart->hal_handle.Init.WordLength = UART_WORDLENGTH_9B;
		break;
	case OVE_UART_PARITY_EVEN:
		uart->hal_handle.Init.Parity = UART_PARITY_EVEN;
		uart->hal_handle.Init.WordLength = UART_WORDLENGTH_9B;
		break;
	default:
		uart->hal_handle.Init.Parity = UART_PARITY_NONE;
		uart->hal_handle.Init.WordLength = UART_WORDLENGTH_8B;
		break;
	}

	if (HAL_UART_Init(&uart->hal_handle) != HAL_OK)
		return OVE_ERR_NOT_SUPPORTED;

	uart_instances[cfg->instance] = uart;
	return OVE_OK;
}

void ove_hal_uart_close(ove_uart_t uart)
{
	if (uart->instance < UART_MAX_INSTANCES)
		uart_instances[uart->instance] = NULL;
	HAL_UART_DeInit(&uart->hal_handle);
}

int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint32_t timeout_ms,
		    size_t *bytes_written)
{
	HAL_StatusTypeDef ret;

	ret = HAL_UART_Transmit(&uart->hal_handle, (uint8_t *)data, (uint16_t)len, timeout_ms);
	if (ret == HAL_OK) {
		if (bytes_written != NULL)
			*bytes_written = len;
		return OVE_OK;
	}
	if (ret == HAL_TIMEOUT)
		return OVE_ERR_TIMEOUT;
	return OVE_ERR_BUS_ERROR;
}

int ove_hal_uart_rx_enable(ove_uart_t uart)
{
	/* Enable RXNE interrupt */
	__HAL_UART_ENABLE_IT(&uart->hal_handle, UART_IT_RXNE);
	HAL_NVIC_SetPriority(instance_to_irqn(uart->instance), 6, 0);
	HAL_NVIC_EnableIRQ(instance_to_irqn(uart->instance));
	return OVE_OK;
}

int ove_hal_uart_tx_flush(ove_uart_t uart)
{
	/* Wait for TC (Transmission Complete) flag */
	while (!__HAL_UART_GET_FLAG(&uart->hal_handle, UART_FLAG_TC))
		;
	return OVE_OK;
}

/* ── ISR handlers ────────────────────────────────────────────────── */

static void uart_irq_handler(unsigned int instance)
{
	ove_uart_t uart = uart_instances[instance];
	if (uart == NULL)
		return;

	if (__HAL_UART_GET_FLAG(&uart->hal_handle, UART_FLAG_RXNE)) {
		uint8_t byte = (uint8_t)(uart->hal_handle.Instance->RDR & 0xFF);
		ove_uart_rx_isr_push(uart, &byte, 1);
	}

	/* Clear overrun if set */
	if (__HAL_UART_GET_FLAG(&uart->hal_handle, UART_FLAG_ORE))
		__HAL_UART_CLEAR_OREFLAG(&uart->hal_handle);
}

void USART1_IRQHandler(void)
{
	uart_irq_handler(0);
}
void USART2_IRQHandler(void)
{
	uart_irq_handler(1);
}
void USART3_IRQHandler(void)
{
	uart_irq_handler(2);
}
#ifdef UART4
void UART4_IRQHandler(void)
{
	uart_irq_handler(3);
}
#endif

#endif /* CONFIG_OVE_UART */
