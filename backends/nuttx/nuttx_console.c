/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/console.h"
#include "ove_backend_common.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
/* NuttX owns USART1 RX through its IRQ-filled /dev/console FIFO. The first
 * non-blocking read claims a separate raw descriptor so personality input does
 * not race the one-byte hardware RDR or inherit canonical/echo processing. */
static int g_console_rfd = -1;

static void console_nonblocking_init(void)
{
	if (g_console_rfd >= 0)
		return;
	g_console_rfd = open("/dev/console", O_RDONLY | O_NONBLOCK);
#if defined(CONFIG_SERIAL_TERMIOS)
	if (g_console_rfd >= 0) {
		struct termios t;
		if (tcgetattr(g_console_rfd, &t) == 0) {
			t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
			t.c_iflag &= ~(tcflag_t)(ICRNL | INLCR | IGNCR | IXON);
			t.c_cc[VMIN] = 0;
			t.c_cc[VTIME] = 0;
			(void)tcsetattr(g_console_rfd, TCSANOW, &t);
		}
	}
#endif
}

#define OVE_NX_USART1 0x40011000u
#define OVE_NX_U1_ISR (*(volatile uint32_t *)(OVE_NX_USART1 + 0x1Cu))
#define OVE_NX_U1_TDR (*(volatile uint32_t *)(OVE_NX_USART1 + 0x28u))
#endif

int ove_console_init(void)
{
	return OVE_OK;
}

int ove_console_getchar(void)
{
	return getchar();
}

int ove_console_try_getchar(void)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	console_nonblocking_init();
	unsigned char c;
	return g_console_rfd >= 0 && read(g_console_rfd, &c, 1) == 1 ? (int)c : -1;
#else
	return -1;
#endif
}

void ove_console_putchar(int c)
{
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO)
	while (!(OVE_NX_U1_ISR & (1u << 7))) {
	}
	OVE_NX_U1_TDR = (unsigned char)c;
#else
	putchar(c);
#endif
}

void ove_console_write(const char *buf, unsigned int len)
{
	printf("%.*s", (int)len, buf);
	(void)fflush(stdout);
}
