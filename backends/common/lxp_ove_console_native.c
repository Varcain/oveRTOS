/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LXP console transport over the board's ordinary oveRTOS console.
 */

#include "ove/hal/hal_lxp_console.h"

#include "ove/types.h"

static int g_ready_events;

int ove_hal_lxp_console_init(void)
{
	/* Capability discovery also withdraws any stale subscriber left by an
	 * aborted launch. Backends without an event source reject the probe. */
	g_ready_events = ove_console_set_ready_callback(NULL, NULL) == OVE_OK;
	return OVE_OK;
}

int ove_hal_lxp_console_try_getchar(void)
{
	return ove_console_try_getchar();
}

void ove_hal_lxp_console_putchar(int c)
{
	ove_console_putchar(c);
}

void ove_hal_lxp_console_guest_write(const char *buf, size_t len)
{
#if defined(OVE_LXP_CONSOLE_NATIVE_TRANSLATES_NEWLINES)
	/* The STM32 FreeRTOS console owns bounded, serialized CRLF translation;
	 * retain its bulk path rather than taking the UART mutex per byte. */
	ove_console_write(buf, (unsigned int)len);
#else
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n')
			ove_console_putchar('\r');
		ove_console_putchar((unsigned char)buf[i]);
	}
#endif
}

int ove_hal_lxp_console_ready_events(void)
{
	return g_ready_events;
}

int ove_hal_lxp_console_set_ready_callback(ove_console_ready_fn callback, const void *context)
{
	if (!g_ready_events)
		return OVE_ERR_NOT_SUPPORTED;
	return ove_console_set_ready_callback(callback, context);
}
