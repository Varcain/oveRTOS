/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Native RTOS <-> Linux guest round-trip scenario.
 */

#include "roundtrip.h"

#include <stdio.h>
#include <string.h>

#include "ove/lxp_console.h"
#include "ove/time.h"
#include "ove/types.h"

#define N_READINGS 3
#define LINE_CAPACITY 56

struct roundtrip_line {
	char text[LINE_CAPACITY];
};

static struct roundtrip_line g_feed_lines[N_READINGS];
static volatile int g_feed_idx;
static volatile int g_feed_ready;
static volatile int g_linux_done;
static volatile int g_worker_exited;
static char g_round_trip[N_READINGS][LINE_CAPACITY];
static volatile int g_round_trip_n;

static ove_thread_t g_worker;
OVE_THREAD_DEFINE(g_worker_storage, 2048);

static uint32_t uptime_ms(void)
{
	uint64_t us = 0;
	(void)ove_time_get_us(&us);
	return (uint32_t)(us / 1000u);
}

static void roundtrip_worker(void *arg)
{
	(void)arg;
	for (int i = 1; i <= N_READINGS; i++) {
		(void)snprintf(g_feed_lines[i - 1].text, sizeof(g_feed_lines[i - 1].text),
			       "reading-%d\n", i);
		ove_lxp_console_printf("[rtos-feeder] -> Linux: reading-%d\n", i);
	}
	g_feed_ready = 1;

	int printed = 0;
	for (;;) {
		while (printed < g_round_trip_n) {
			ove_lxp_console_printf(
				"[rtos-consumer] <- Linux (round trip #%d @ %u ms): \"%s\"\n",
				printed + 1, (unsigned int)uptime_ms(), g_round_trip[printed]);
			printed++;
		}
		if (g_linux_done && printed >= g_round_trip_n)
			break;
		ove_time_delay_ms(50);
	}
	g_worker_exited = 1;
}

static long feed_read(void *ctx, int fd, void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	if (g_feed_idx >= N_READINGS)
		return 0;
	const char *src = g_feed_lines[g_feed_idx++].text;
	size_t size = strlen(src);
	if (size > len)
		size = len;
	memcpy(buf, src, size);
	return (long)size;
}

static long consume_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	char text[LINE_CAPACITY];
	size_t size = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
	memcpy(text, buf, size);
	while (size && (text[size - 1] == '\n' || text[size - 1] == '\r'))
		size--;
	text[size] = 0;
	if (size) {
		int index = g_round_trip_n;
		if (index < N_READINGS) {
			memcpy(g_round_trip[index], text, size + 1);
			__asm__ volatile("" ::: "memory");
			g_round_trip_n = index + 1;
		}
	}
	return (long)len;
}

int linux_interop_roundtrip_prepare(ove_lxp_launch_config_t *config)
{
	if (!config)
		return OVE_ERR_INVALID_PARAM;
	memset(g_feed_lines, 0, sizeof(g_feed_lines));
	memset(g_round_trip, 0, sizeof(g_round_trip));
	g_feed_idx = 0;
	g_feed_ready = 0;
	g_linux_done = 0;
	g_worker_exited = 0;
	g_round_trip_n = 0;

	config->write_fn = consume_write;
	config->read_fn = feed_read;
	config->io_ctx = NULL;
	int rc = ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", roundtrip_worker,
				 NULL, OVE_PRIO_LOW, sizeof(g_worker_storage_stack),
				 g_worker_storage_stack);
	if (rc != OVE_OK)
		return rc;
	while (!g_feed_ready)
		ove_thread_sleep_ms(1);
	return OVE_OK;
}

int linux_interop_roundtrip_complete(int guest_status)
{
	g_linux_done = 1;
	while (!g_worker_exited)
		ove_thread_sleep_ms(1);
	(void)ove_thread_deinit(g_worker);

	int valid = guest_status >= 0 && g_round_trip_n == N_READINGS;
	for (int i = 0; valid && i < N_READINGS; i++) {
		char expected[16];
		(void)snprintf(expected, sizeof(expected), "reading-%d", i + 1);
		valid = strcmp(g_round_trip[i], expected) == 0;
	}
	if (!valid) {
		ove_lxp_console_printf(
			"[demo] FAIL: phase-1 round trip mismatch rc=%d received=%d\n",
			guest_status, g_round_trip_n);
		return OVE_ERR_IO;
	}
	ove_lxp_console_write(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");
	return OVE_OK;
}

ove_thread_t linux_interop_roundtrip_worker(void)
{
	return g_worker;
}

size_t linux_interop_roundtrip_worker_stack_size(void)
{
	return sizeof(g_worker_storage_stack);
}
