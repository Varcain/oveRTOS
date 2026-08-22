/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * oveRTOS-owned system-console provider for LXP. Linux tty semantics stay in
 * LXP; this adapter owns non-consuming readiness lookahead and binds the
 * board-selected physical transport to each personality run.
 */

#include "ove/lxp_console.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ove/hal/hal_lxp_console.h"
#include "ove/types.h"

/* The selected transport owns its RX FIFO. One byte of lookahead keeps
 * readiness probing non-consuming. */
static int g_lookahead = -1;
static int g_ready_events;

static void native_write_char(char c)
{
	ove_hal_lxp_console_putchar((unsigned char)c);
}

static int native_ready(void)
{
	if (g_lookahead < 0)
		g_lookahead = ove_hal_lxp_console_try_getchar();
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
	ove_hal_lxp_console_guest_write(buf, len);
	return (long)len;
}

static int console_poll(void *ctx)
{
	(void)ctx;
	return native_ready();
}

static int console_subscribe(void *ctx, ove_lxp_console_ready_fn ready, const void *ready_context)
{
	(void)ctx;
	return ove_hal_lxp_console_set_ready_callback(ready, ready_context) == OVE_OK
		       ? OVE_OK
		       : OVE_ERR_NOT_SUPPORTED;
}

static void console_unsubscribe(void *ctx)
{
	(void)ctx;
	(void)ove_hal_lxp_console_set_ready_callback(NULL, NULL);
}

int ove_lxp_console_init(void)
{
	int rc = ove_hal_lxp_console_init();
	if (rc != OVE_OK)
		return rc;
	g_ready_events = ove_hal_lxp_console_ready_events();
	(void)native_ready();
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
	config->console_subscribe = g_ready_events ? console_subscribe : NULL;
	config->console_unsubscribe = g_ready_events ? console_unsubscribe : NULL;
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
