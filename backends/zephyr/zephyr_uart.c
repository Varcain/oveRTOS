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
#include <zephyr/drivers/uart.h>

/* Forward declaration of the portable ISR push helper */
extern void ove_uart_rx_isr_push(ove_uart_t uart, const void *data, size_t len);

static const struct device *instance_to_dev(unsigned int instance)
{
	switch (instance) {
	case 0:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(usart1));
	case 1:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(usart2));
	case 2:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(usart3));
	case 3:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart4));
	default:
		return NULL;
	}
}

/* Zephyr UART IRQ callback */
static void zephyr_uart_irq_cb(const struct device *dev, void *user_data)
{
	ove_uart_t uart = (ove_uart_t)user_data;
	uint8_t byte;

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			while (uart_fifo_read(dev, &byte, 1) == 1)
				ove_uart_rx_isr_push(uart, &byte, 1);
		}
	}
}

int ove_hal_uart_open(ove_uart_t uart, const struct ove_uart_cfg *cfg)
{
	const struct device *dev = instance_to_dev(cfg->instance);

	if (dev == NULL || !device_is_ready(dev))
		return OVE_ERR_INVALID_PARAM;

	uart->dev = dev;

	struct uart_config ucfg = {
		.baudrate = cfg->baudrate,
		.data_bits = UART_CFG_DATA_BITS_8,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};

	switch (cfg->data_bits) {
	case 7:
		ucfg.data_bits = UART_CFG_DATA_BITS_7;
		break;
	case 9:
		ucfg.data_bits = UART_CFG_DATA_BITS_9;
		break;
	default:
		break;
	}

	switch (cfg->parity) {
	case OVE_UART_PARITY_ODD:
		ucfg.parity = UART_CFG_PARITY_ODD;
		break;
	case OVE_UART_PARITY_EVEN:
		ucfg.parity = UART_CFG_PARITY_EVEN;
		break;
	default:
		break;
	}

	if (cfg->stop_bits == OVE_UART_STOP_2)
		ucfg.stop_bits = UART_CFG_STOP_BITS_2;

	if (cfg->flow_control == OVE_UART_FLOW_RTS_CTS)
		ucfg.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS;

	int ret = uart_configure(dev, &ucfg);
	if (ret != 0)
		return OVE_ERR_NOT_SUPPORTED;

	return OVE_OK;
}

void ove_hal_uart_close(ove_uart_t uart)
{
	if (uart->dev != NULL) {
		uart_irq_rx_disable(uart->dev);
		uart->dev = NULL;
	}
}

int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_written)
{
	const uint8_t *p = data;
	size_t i;
	(void)timeout_ns;

	for (i = 0; i < len; i++)
		uart_poll_out(uart->dev, p[i]);

	if (bytes_written != NULL)
		*bytes_written = len;
	return OVE_OK;
}

int ove_hal_uart_rx_enable(ove_uart_t uart)
{
	uart_irq_callback_user_data_set(uart->dev, zephyr_uart_irq_cb, uart);
	uart_irq_rx_enable(uart->dev);
	return OVE_OK;
}

int ove_hal_uart_tx_flush(ove_uart_t uart)
{
	/* Zephyr poll_out blocks per-character, so TX is already flushed */
	(void)uart;
	return OVE_OK;
}

#endif /* CONFIG_OVE_UART */
