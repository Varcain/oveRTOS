/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo.
 *
 * One firmware image, two worlds, two phases, using only engine-neutral
 * oveRTOS thread, time, socket, console, and Linux-host APIs.
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (ove_thread) feeds
 *  three readings into the rootfs-owned guest mode and drains its replies:
 *      RTOS feeder -> read cb -> guest roundtrip -> write cb -> RTOS consumer
 *
 *  Phase 2 — INTERACTIVE shell. The same rootfs entrypoint boots userspace;
 *  type commands (ls /, echo hi, cat /etc/hostname, ...) and
 *  `exit` to finish.
 *
 * Guest callbacks run in the privileged, preemptible coordinator. Fixed staging
 * keeps the native round trip allocation-free.
 */

#include <stdio.h>
#include <string.h>

#include "ove/lxp_console.h"
#include "ove/thread.h"
#include "ove/time.h"

#include "ove/app.h"
#if defined(CONFIG_OVE_LINUX_NET)
#include "ove/net.h" /* product-level socket smoke over the initialized host network */
#endif

#include "ove_config.h"
#include "ove/build.h" /* OVE_BUILD_ID — generated revisions with honest fallbacks */
#include "qualification.h"
#include "rt_scope.h"

#define GUEST_ENTRYPOINT "/usr/libexec/ove-interop-guest"

static uint32_t uptime_ms(void)
{
	uint64_t us = 0;
	(void)ove_time_get_us(&us);
	return (uint32_t)(us / 1000u);
}

#if defined(CONFIG_OVE_LINUX_NET)
/*
 * Allow a bounded post-phase-1 readiness window: publishing a static address
 * can precede carrier, worker, and ARP readiness.
 */
static void network_transport_smoke(const ove_lxp_host_t *host)
{
	static ove_socket_storage_t s_sk;
	ove_sockaddr_t peer = {0};
	if (ove_lxp_host_netif_get_addr(host, NULL, &peer, NULL) != OVE_OK) {
		ove_lxp_console_write("[demo] socket smoke: configured gateway unavailable\n");
		return;
	}
	peer.port = 22;
	const uint32_t started_ms = uptime_ms();
	int last_rc = -1;
	for (uint32_t attempt = 1; attempt <= 12; attempt++) {
		ove_socket_t sk = NULL;
		last_rc = ove_socket_open(&sk, &s_sk, OVE_AF_INET, OVE_SOCK_STREAM);
		if (last_rc != OVE_OK)
			goto retry;

		last_rc = ove_socket_connect(sk, &peer, OVE_MS(500));
		if (last_rc != OVE_OK) {
			ove_socket_close(sk);
			goto retry;
		}

		char rb[80];
		size_t got = 0;
		last_rc = ove_socket_recv(sk, rb, sizeof(rb) - 1, &got, OVE_SEC(1));
		if (last_rc == OVE_OK && got > 0) {
			for (size_t i = 0; i < got; i++)
				if (rb[i] == '\r' || rb[i] == '\n')
					rb[i] = 0;
			rb[got < sizeof(rb) ? got : sizeof(rb) - 1] = 0;
			ove_lxp_console_printf(
				"[demo] socket smoke (post-phase1) OK <- %u.%u.%u.%u:22: %s "
				"(ready after %u ms, attempt %u)\n",
				(unsigned int)peer.addr[0], (unsigned int)peer.addr[1],
				(unsigned int)peer.addr[2], (unsigned int)peer.addr[3], rb,
				(unsigned int)(uptime_ms() - started_ms), (unsigned int)attempt);
			ove_socket_close(sk);
			return;
		}
		ove_socket_close(sk);

	retry:
		if (attempt < 12)
			ove_thread_sleep_ms(250);
	}

	ove_lxp_console_printf(
		"[demo] socket smoke (post-phase1): not ready after %u ms, last rc=%d\n",
		(unsigned int)(uptime_ms() - started_ms), last_rc);
}
#endif

/* ---- RTOS <-> Linux bridge: fixed, allocation-free staging ---------------- */
struct lnx_line {
	char text[56];
};
#define N_READINGS 3

/* Callbacks run in the privileged coordinator. Pre-staging makes an exhausted
 * feed unambiguously mean EOF while retaining a concurrent native worker. */
static struct lnx_line g_feed_lines[N_READINGS]; /* RTOS -> Linux: readings staged up front */
static volatile int g_feed_idx;			 /* feed_read cursor; == N_READINGS => EOF */
static volatile int g_feed_ready;		 /* all feed lines queued (so no premature EOF) */
static volatile int g_linux_done;		 /* the phase-1 program has exited */
static volatile int g_worker_exited;		 /* the worker thread has returned */
static char g_round_trip[N_READINGS][56]; /* what came back through Linux (for the verdict) */
static volatile int g_round_trip_n;

/* ---- the native RTOS worker thread (feeds, then consumes) ------------------ */
static ove_thread_t g_worker;
OVE_THREAD_DEFINE(g_worker_storage, 2048);

static void rtos_worker(void *arg)
{
	(void)arg;

	/* Stage every reading before launch, then let feed_read serve them by index. */
	for (int i = 1; i <= N_READINGS; i++) {
		(void)snprintf(g_feed_lines[i - 1].text, sizeof(g_feed_lines[i - 1].text),
			       "reading-%d\n", i);
		ove_lxp_console_printf("[rtos-feeder] -> Linux: reading-%d\n", i);
	}
	g_feed_ready = 1;

	/* consume_write publishes each reply before advancing g_round_trip_n. */
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
		/* Poll coarsely so this low-priority demonstration worker remains idle
		 * while the coordinator loads or executes guest work. */
		ove_time_delay_ms(50);
	}
	g_worker_exited = 1;
}

/* ---- phase 1 callbacks ----------------------------------------------------- */
static long feed_read(void *ctx, int fd, void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	if (g_feed_idx >= N_READINGS)
		return 0; /* EOF: every staged reading has been served */
	const char *src = g_feed_lines[g_feed_idx++].text;
	size_t l = strlen(src);
	if (l > len)
		l = len;
	memcpy(buf, src, l);
	return (long)l;
}

/* Capture each guest stdout line for the native consumer. */
static long consume_write(void *ctx, int fd, const void *buf, size_t len)
{
	(void)ctx;
	(void)fd;
	char tmp[56];
	size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
	memcpy(tmp, buf, n);
	while (n && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r'))
		n--;
	tmp[n] = 0;
	if (n) {
		int idx = g_round_trip_n;
		if (idx < N_READINGS) {
			memcpy(g_round_trip[idx], tmp, n + 1);
			__asm__ volatile("" ::: "memory"); /* publish the text before the count */
			g_round_trip_n = idx + 1; /* the worker prints replies as this advances */
		}
	}
	return (long)len;
}

/* One parsed host is reused by both rootfs guest modes. */
static ove_lxp_host_t g_linux_host;

static int run_guest_mode(const ove_lxp_launch_config_t *config, const char *mode)
{
	const char *const argv[] = {"ove-interop-guest", mode, NULL};
	return ove_lxp_host_run(&g_linux_host, config, GUEST_ENTRYPOINT, 2, argv);
}

static void demo_exit(unsigned int code)
{
	ove_lxp_host_deinit(&g_linux_host);
	ove_app_exit(code);
}

static ove_thread_t g_demo;
/* The O0 QEMU integration build uses 4320 bytes of this coordinator stack;
 * retain nearly another call-chain of margin in backend-safe host memory. */
OVE_THREAD_DEFINE_HOST(g_demo_storage, 8192);

static void demo_body(void *arg)
{
	(void)arg;
	(void)ove_lxp_console_init();
	ove_lxp_console_write(
		"=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");
	ove_lxp_console_write("[build] " OVE_BUILD_ID "\n");

#if defined(CONFIG_OVE_LINUX_RT_SCOPE)
	if (linux_rt_scope_start(ove_lxp_console_write) != OVE_OK) {
		ove_lxp_console_write("[rt-scope] FAIL: timer/event/thread setup\n");
		demo_exit(1);
	}
	ove_lxp_console_write("[rt-scope] CH1=D3/PB4 TIM3 1kHz reference; "
			      "CH2=D4/PG7 critical-thread response\n");
#endif

	linux_interop_qualification_start();

	/* Build configuration owns rootfs placement and product network topology;
	 * the host facade owns native provider setup, rollback, and teardown. */
	int host_rc = ove_lxp_host_init(&g_linux_host);
	if (host_rc != OVE_OK) {
		ove_lxp_console_printf("[demo] FAIL: Linux host init failed rc=%d\n", host_rc);
		demo_exit(1);
	}

#if defined(CONFIG_OVE_LINUX_NET)
	ove_sockaddr_t ip = {0}, gw = {0};
	if (ove_lxp_host_netif_get_addr(&g_linux_host, &ip, &gw, NULL) == OVE_OK) {
		ove_lxp_console_printf("[demo] eth0 up ip=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
				       (unsigned int)ip.addr[0], (unsigned int)ip.addr[1],
				       (unsigned int)ip.addr[2], (unsigned int)ip.addr[3],
				       (unsigned int)gw.addr[0], (unsigned int)gw.addr[1],
				       (unsigned int)gw.addr[2], (unsigned int)gw.addr[3]);
	} else {
		ove_lxp_console_write("[demo] eth0 address unavailable after bring-up\n");
	}
#endif

	/* ---- Phase 1: rootfs-owned bidirectional round trip ------------------- */
	ove_lxp_console_write("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	/* A lower-priority native worker runs whenever this coordinator blocks. */
	if (ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", rtos_worker, NULL,
			    OVE_PRIO_LOW, sizeof(g_worker_storage_stack),
			    g_worker_storage_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: ove_thread_init\n");
		demo_exit(1);
	}
	while (!g_feed_ready)
		ove_thread_sleep_ms(1); /* blocking sleep yields to the lower-priority worker */

	ove_lxp_launch_config_t cfg1 = {
		.write_fn = consume_write,
		.read_fn = feed_read,
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	ove_lxp_console_bind_diagnostics(&cfg1);
	ove_lxp_console_write("[demo] launching the Linux guest round-trip mode...\n");
	int rc1 = run_guest_mode(&cfg1, "roundtrip");

	g_linux_done = 1;
	while (!g_worker_exited)
		ove_thread_sleep_ms(1);
	(void)ove_thread_deinit(g_worker);

	int ok = (rc1 >= 0) && (g_round_trip_n == N_READINGS);
	for (int i = 0; ok && i < N_READINGS; i++) {
		char want[16];
		(void)snprintf(want, sizeof(want), "reading-%d", i + 1);
		ok = (strcmp(g_round_trip[i], want) == 0);
	}
	if (!ok) {
		ove_lxp_console_printf(
			"[demo] FAIL: phase-1 round trip mismatch rc=%d received=%d\n", rc1,
			g_round_trip_n);
		demo_exit(1);
	}
	ove_lxp_console_write(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");

#if defined(CONFIG_OVE_LINUX_NET)
	network_transport_smoke(&g_linux_host);
#endif

	linux_interop_qualification_arm_guest_tests();

	/* ---- Phase 2: boot userspace or run the hard-float context regression - */
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	ove_lxp_console_write("\n-- phase 2: hard-float guest context self-test --\n");
#else
	ove_lxp_console_write("\n-- phase 2: booting uClinux (BusyBox init -> rcS -> login shell;"
			      " run commands, `poweroff` to halt) --\n");
#endif
	ove_lxp_launch_config_t cfg2 = {
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	ove_lxp_console_bind_diagnostics(&cfg2);
	ove_lxp_console_bind(&cfg2);
	int rc2;
	if (linux_interop_qualification_measurement_start() != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: latency monitor thread init\n");
		demo_exit(1);
	}
#if defined(CONFIG_OVE_LINUX_GUEST_FP_SELFTEST)
	rc2 = run_guest_mode(&cfg2, "fpcheck");
	ove_lxp_console_write("\n=== interop demo done (hard-float self-test exited) ===\n");
#else
	rc2 = run_guest_mode(&cfg2, "boot");
	ove_lxp_console_write("\n=== interop demo done (uClinux halted) ===\n");
#endif
	linux_interop_qualification_measurement_stop();
	ove_lxp_host_observation_t observation;
	if (ove_lxp_host_observe(&g_linux_host, &observation) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: post-run LXP observation unavailable\n");
		demo_exit(1);
	}
	const linux_interop_thread_audit_t audit_threads[] = {
		{"coordinator", g_demo, sizeof(g_demo_storage_stack)},
		{"worker", g_worker, sizeof(g_worker_storage_stack)},
	};
	linux_interop_qualification_report(&observation, audit_threads,
					   sizeof(audit_threads) / sizeof(audit_threads[0]));
	demo_exit(rc2 >= 0 ? 0 : 1);
}

void ove_main(void)
{
	if (ove_thread_init(&g_demo, &g_demo_storage, "demo", demo_body, NULL, OVE_PRIO_NORMAL,
			    sizeof(g_demo_storage_stack), g_demo_storage_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: demo thread init\n");
		demo_exit(1);
	}
	ove_run();
}
