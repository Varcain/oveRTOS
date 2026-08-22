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
#include "ove/thread.h"
#include "ove/time.h"
#include "ove/types.h"

#include "qualification.h"

#define N_READINGS 3
#define LINE_CAPACITY 56

struct roundtrip_line {
	char text[LINE_CAPACITY];
};

static struct roundtrip_line g_feed_lines[N_READINGS];
static int g_feed_idx;
static char g_round_trip[N_READINGS][LINE_CAPACITY];
static int g_round_trip_n;

static ove_thread_t g_worker;
OVE_THREAD_DEFINE(g_worker_storage, 2048);

static void roundtrip_worker(void *arg)
{
	(void)arg;
	ove_thread_t self = ove_thread_get_self();
	int printed = 0;
	for (;;) {
		int available = __atomic_load_n(&g_round_trip_n, __ATOMIC_ACQUIRE);
		while (printed < available) {
			ove_lxp_console_printf(
				"[rtos-consumer] <- Linux (round trip #%d @ %u ms): \"%s\"\n",
				printed + 1, (unsigned int)(ove_time_now_steady_ns() / OVE_MS(1)),
				g_round_trip[printed]);
			printed++;
		}
		if (ove_thread_should_stop(self) && printed >= available)
			break;
		ove_thread_sleep_ms(50);
	}
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
		int index = __atomic_load_n(&g_round_trip_n, __ATOMIC_RELAXED);
		if (index < N_READINGS) {
			memcpy(g_round_trip[index], text, size + 1);
			__atomic_store_n(&g_round_trip_n, index + 1, __ATOMIC_RELEASE);
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
	__atomic_store_n(&g_round_trip_n, 0, __ATOMIC_RELAXED);
	for (int i = 1; i <= N_READINGS; i++) {
		(void)snprintf(g_feed_lines[i - 1].text, sizeof(g_feed_lines[i - 1].text),
			       "reading-%d\n", i);
		ove_lxp_console_printf("[rtos-feeder] -> Linux: reading-%d\n", i);
	}

	config->write_fn = consume_write;
	config->read_fn = feed_read;
	config->io_ctx = NULL;
	return OVE_THREAD_INIT_DEFINED(g_worker, g_worker_storage, "rtos-worker", roundtrip_worker,
				       NULL, OVE_PRIO_LOW);
}

int linux_interop_roundtrip_complete(int guest_status)
{
	ove_thread_request_stop(g_worker);
	while (ove_thread_get_state(g_worker) != OVE_THREAD_STATE_TERMINATED)
		ove_thread_sleep_ms(1);
	linux_interop_qualification_observe_thread("worker", g_worker,
						   sizeof(g_worker_storage_stack));
	(void)ove_thread_deinit(g_worker);

	int received = __atomic_load_n(&g_round_trip_n, __ATOMIC_ACQUIRE);
	int valid = guest_status == 0 && received == N_READINGS;
	for (int i = 0; valid && i < N_READINGS; i++) {
		char expected[16];
		(void)snprintf(expected, sizeof(expected), "reading-%d", i + 1);
		valid = strcmp(g_round_trip[i], expected) == 0;
	}
	if (!valid) {
		ove_lxp_console_printf(
			"[demo] FAIL: phase-1 round trip mismatch rc=%d received=%d\n",
			guest_status, received);
		return OVE_ERR_IO;
	}
	ove_lxp_console_write(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");
	return OVE_OK;
}
