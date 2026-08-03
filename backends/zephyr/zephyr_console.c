/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include "ove_config.h"
#if defined(CONFIG_OVE_LINUX)
#include "lxp/lxp_run.h"
#endif
#include <zephyr/console/console.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Own RX after Zephyr finishes setting up its boot console. The backend keeps
 * a bounded IRQ-filled FIFO and publishes readiness to the Linux coordinator;
 * output remains a bounded polled TDR path. A raw RDR poll without a wake
 * source strands a guest parked in read(2) once the coordinator goes idle. */
#define OVE_Z_USART1 0x40011000u
#define OVE_Z_U1_ISR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x1Cu))
#define OVE_Z_U1_TDR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x28u))
#define OVE_Z_RX_BUFFER_SIZE 128u

static const struct device *const g_console_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static uint8_t g_console_rx[OVE_Z_RX_BUFFER_SIZE];
static volatile uint16_t g_console_rx_head;
static volatile uint16_t g_console_rx_tail;
static int g_console_rx_claimed;

static void console_rx_irq(const struct device *dev, void *user_data)
{
	(void)user_data;
	int published = 0;
	if (!uart_irq_update(dev))
		return;
	while (uart_irq_rx_ready(dev)) {
		uint8_t byte;
		if (uart_fifo_read(dev, &byte, 1) != 1)
			break;
		uint16_t head = g_console_rx_head;
		uint16_t next = (uint16_t)((head + 1u) % OVE_Z_RX_BUFFER_SIZE);
		if (next != g_console_rx_tail) {
			g_console_rx[head] = byte;
			g_console_rx_head = next;
			published = 1;
		}
	}
#if defined(CONFIG_OVE_LINUX)
	if (published)
		lxp_console_kick();
#else
	(void)published;
#endif
}

static void console_claim_rx(void)
{
	if (!g_console_rx_claimed) {
		uart_irq_rx_disable(g_console_uart);
		(void)uart_irq_callback_user_data_set(g_console_uart, console_rx_irq, NULL);
		uart_irq_rx_enable(g_console_uart);
		g_console_rx_claimed = 1;
	}
}
#endif

int ove_console_init(void)
{
	console_init();
	return OVE_OK;
}

int ove_console_getchar(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	int c;
	while ((c = ove_console_try_getchar()) < 0) {
	}
	return c;
#else
	return (int)console_getchar();
#endif
}

int ove_console_try_getchar(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	console_claim_rx();
	unsigned int key = irq_lock();
	uint16_t tail = g_console_rx_tail;
	int c = -1;
	if (tail != g_console_rx_head) {
		c = g_console_rx[tail];
		g_console_rx_tail = (uint16_t)((tail + 1u) % OVE_Z_RX_BUFFER_SIZE);
	}
	irq_unlock(key);
	return c;
#else
	return -1;
#endif
}

void ove_console_putchar(int c)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	while (!(OVE_Z_U1_ISR & (1u << 7))) {
	}
	OVE_Z_U1_TDR = (unsigned char)c;
#else
	printk("%c", (char)c);
#endif
}

void ove_console_write(const char *buf, unsigned int len)
{
	printk("%.*s", (int)len, buf);
}
