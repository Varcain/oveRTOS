/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned system-console provider for LXP. Linux tty semantics stay in
 * LXP; this adapter owns the concrete UART transport, non-consuming readiness
 * lookahead, newline policy, and run-scoped native readiness subscription.
 */

#include "ove/lxp_console.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ove/console.h"
#include "ove/types.h"
#include "ove_config.h"

#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)

/* QEMU's engine console occupies UART0. The guest system console therefore
 * uses CMSDK UART1 (`-serial none -serial stdio`) directly from privileged
 * coordinator context. */
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#define OVE_LXP_UART_BASE 0x50201000u /* AN521 UART1, secure peripheral region */
#else
#define OVE_LXP_UART_BASE 0x40005000u /* AN500 UART1 */
#endif
#define OVE_LXP_UART_REG(off) (*(volatile unsigned int *)(OVE_LXP_UART_BASE + (off)))

static void native_write_char(char c)
{
	while (OVE_LXP_UART_REG(0x04) & 1u) {
	}
	OVE_LXP_UART_REG(0x00) = (unsigned char)c;
}

static int native_ready(void)
{
	return (OVE_LXP_UART_REG(0x04) & 2u) ? 1 : 0;
}

static int native_read_char(void)
{
	while (!native_ready()) {
	}
	return (int)(OVE_LXP_UART_REG(0x00) & 0xffu);
}

#else

/* The ordinary console owns its RX FIFO. One byte of lookahead keeps
 * readiness probing non-consuming. */
static int g_lookahead = -1;

static void native_write_char(char c)
{
	ove_console_putchar((unsigned char)c);
}

static int native_ready(void)
{
	if (g_lookahead < 0)
		g_lookahead = ove_console_try_getchar();
	return g_lookahead >= 0;
}

static int native_read_char(void)
{
	while (!native_ready()) {
	}
	int c = g_lookahead;
	g_lookahead = -1;
	return c;
}

#endif

static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	if (len == 0u)
		return 0;
	int c = native_read_char();
	if (c < 0)
		return 0;
	if (c == '\n')
		c = '\r';
	*(char *)buf = (char)c;
	return 1;
}

static long console_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && defined(CONFIG_OVE_RTOS_FREERTOS)
	ove_console_write((const char *)buf, (unsigned int)len);
#else
	const char *bytes = buf;
	for (size_t i = 0; i < len; i++) {
		if (bytes[i] == '\n')
			native_write_char('\r');
		native_write_char(bytes[i]);
	}
#endif
	return (long)len;
}

static int console_poll(void *ctx)
{
	(void)ctx;
	return native_ready();
}

#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(defined(CONFIG_OVE_RTOS_FREERTOS) || defined(CONFIG_OVE_RTOS_ZEPHYR))
static int console_subscribe(void *ctx, ove_lxp_console_ready_fn ready,
			     const void *ready_context)
{
	(void)ctx;
	return ove_console_set_ready_callback(ready, ready_context) == OVE_OK
		       ? OVE_OK
		       : OVE_ERR_NOT_SUPPORTED;
}

static void console_unsubscribe(void *ctx)
{
	(void)ctx;
	(void)ove_console_set_ready_callback(NULL, NULL);
}
#endif

int ove_lxp_console_init(void)
{
#if defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500) || \
	defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN521)
	/* CMSDK UART: BAUDDIV >= 16, TX enable | RX enable. */
	OVE_LXP_UART_REG(0x10) = 16u;
	OVE_LXP_UART_REG(0x08) = 0x3u;
#else
	(void)native_ready();
#endif
	return OVE_OK;
}

void ove_lxp_console_bind(ove_lxp_launch_config_t *config)
{
	if (!config)
		return;
	config->write_fn = console_write;
	config->read_fn = console_read;
	config->console_poll = console_poll;
	config->io_ctx = NULL;
#if defined(CONFIG_OVE_BOARD_STM32F746G_DISCO) && \
	(defined(CONFIG_OVE_RTOS_FREERTOS) || defined(CONFIG_OVE_RTOS_ZEPHYR))
	config->console_subscribe = console_subscribe;
	config->console_unsubscribe = console_unsubscribe;
#else
	config->console_subscribe = NULL;
	config->console_unsubscribe = NULL;
#endif
}

static const char *guest_exit_reason_name(uint8_t reason)
{
	switch (reason) {
	case OVE_LXP_EXIT_REASON_SIGNAL:
		return "signal";
	case OVE_LXP_EXIT_REASON_SIGNAL_DEPTH:
		return "signal-depth";
	case OVE_LXP_EXIT_REASON_MEMORY_FAULT:
		return "memory-fault";
	case OVE_LXP_EXIT_REASON_EXEC_RESOURCE:
		return "exec-resource";
	case OVE_LXP_EXIT_REASON_EXEC_LOAD:
		return "exec-load";
	case OVE_LXP_EXIT_REASON_STATE_CORRUPTION:
		return "state-corruption";
	case OVE_LXP_EXIT_REASON_HOST_TRANSITION:
		return "host-transition";
	default:
		return "unspecified";
	}
}

static void console_report_enosys(long nr)
{
	ove_lxp_console_printf("[lxp] unimplemented syscall nr=%ld\n", nr);
}

static void console_report_guest_exit(const ove_lxp_guest_exit_info_t *info)
{
	if (!info || info->reason == OVE_LXP_EXIT_REASON_NORMAL)
		return;
	ove_lxp_console_printf("[lxp] guest-exit slot=%d pid=%d comm=%s status=%d reason=%s "
			       "signal=%u detail=0x%08lx address=0x%08lx\n",
			       info->slot, info->pid, info->comm ? info->comm : "?", info->status,
			       guest_exit_reason_name(info->reason), (unsigned int)info->signal,
			       (unsigned long)info->detail, (unsigned long)info->address);
}

void ove_lxp_console_bind_diagnostics(ove_lxp_launch_config_t *config)
{
	if (!config)
		return;
	config->on_enosys = console_report_enosys;
	config->on_guest_exit = console_report_guest_exit;
}

void ove_lxp_console_write(const char *text)
{
	if (!text)
		return;
	while (*text)
		native_write_char(*text++);
}

void ove_lxp_console_printf(const char *format, ...)
{
	if (!format)
		return;
	char text[256];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (length > 0)
		ove_lxp_console_write(text);
}
