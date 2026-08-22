/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dedicated Linux-personality console for QEMU MPS2 boards. The engine owns
 * UART0, so QEMU connects this CMSDK UART1 transport to the terminal.
 */

#include "ove/hal/hal_lxp_console.h"

#include <stdint.h>

#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)
#define OVE_LXP_UART_BASE 0x40005000u
#elif defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
#define OVE_LXP_UART_BASE 0x50201000u
#else
#error "The CMSDK LXP console requires a supported QEMU MPS2 board"
#endif

#define OVE_LXP_UART_REG(offset) (*(volatile uint32_t *)(uintptr_t)(OVE_LXP_UART_BASE + (offset)))

int ove_hal_lxp_console_init(void)
{
	/* CMSDK UART: BAUDDIV >= 16, TX enable | RX enable. */
	OVE_LXP_UART_REG(0x10u) = 16u;
	OVE_LXP_UART_REG(0x08u) = 0x3u;
	return OVE_OK;
}

int ove_hal_lxp_console_try_getchar(void)
{
	return (OVE_LXP_UART_REG(0x04u) & 2u) != 0u ? (int)(OVE_LXP_UART_REG(0x00u) & 0xffu) : -1;
}

void ove_hal_lxp_console_putchar(int c)
{
	while ((OVE_LXP_UART_REG(0x04u) & 1u) != 0u) {
	}
	OVE_LXP_UART_REG(0x00u) = (unsigned char)c;
}

void ove_hal_lxp_console_guest_write(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			ove_hal_lxp_console_putchar('\r');
		ove_hal_lxp_console_putchar((unsigned char)buf[i]);
	}
}

int ove_hal_lxp_console_ready_events(void)
{
	return 0;
}

int ove_hal_lxp_console_set_ready_callback(ove_console_ready_fn callback, const void *context)
{
	(void)callback;
	(void)context;
	return OVE_ERR_NOT_SUPPORTED;
}
