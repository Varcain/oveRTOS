/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include <zephyr/console/console.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* Zephyr's console IRQ would consume USART1 RDR before a non-blocking poller
 * sees it. Claim RX on the first try-read; output remains a bounded polled TDR
 * path, matching the personality console's previous behavior. */
#define OVE_Z_USART1 0x40011000u
#define OVE_Z_U1_CR1 (*(volatile uint32_t *)(OVE_Z_USART1 + 0x00u))
#define OVE_Z_U1_ISR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x1Cu))
#define OVE_Z_U1_ICR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x20u))
#define OVE_Z_U1_RDR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x24u))
#define OVE_Z_U1_TDR (*(volatile uint32_t *)(OVE_Z_USART1 + 0x28u))
#define OVE_Z_U1_RXERR ((1u << 1) | (1u << 2) | (1u << 3))
static int g_console_rx_claimed;

static void console_claim_rx(void)
{
	if (!g_console_rx_claimed) {
		OVE_Z_U1_CR1 &= ~(1u << 5);
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
	return (int)console_getchar();
}

int ove_console_try_getchar(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	console_claim_rx();
	uint32_t isr = OVE_Z_U1_ISR;
	if (isr & OVE_Z_U1_RXERR)
		OVE_Z_U1_ICR = OVE_Z_U1_RXERR;
	return (isr & (1u << 5)) ? (int)(OVE_Z_U1_RDR & 0xffu) : -1;
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
