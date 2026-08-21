/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo.
 *
 * One firmware image, two worlds, two phases — built entirely on the
 * engine-agnostic oveRTOS APIs (ove_thread / ove_queue / ove_time) on the RTOS
 * side and the oveRTOS Linux-host facade on the Linux side; no
 * direct Zephyr kernel calls.
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (ove_thread) feeds
 *  three "sensor readings" INTO the rootfs-owned guest demo through its stdin,
 *  and drains what it echoes back
 *  OUT of its stdout:
 *      RTOS feeder -> g_feed_lines[] -> read cb -> [guest roundtrip] -> write cb -> g_round_trip[] -> RTOS consumer
 *
 *  Phase 2 — INTERACTIVE shell. The same rootfs entrypoint boots userspace;
 *  type commands (ls /, echo hi, cat /etc/hostname, ...) and
 *  `exit` to finish.
 *
 * The svc top half only snapshots ordinary syscall registers and parks the guest;
 * I/O callbacks run later in the privileged, preemptible coordinator task. Phase 1
 * keeps its fixed arrays and published indices to avoid allocation and unnecessary
 * scheduler traffic. Phase 2 binds the oveRTOS system-console provider, whose
 * non-consuming readiness probe parks an empty console instead of blocking the
 * coordinator and whose event-capable backends wake it without periodic polling.
 */

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

#ifndef UNUSED
#define UNUSED(x) ((void)(x))
#endif

#define GUEST_ENTRYPOINT "/usr/libexec/ove-interop-guest"

/* Minimal string builders (no libc printf dependency). */
static char *put_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *put_dec(char *p, uint32_t v)
{
	char tmp[10];
	int i = 0;
	if (v == 0) {
		*p++ = '0';
		return p;
	}
	while (v) {
		tmp[i++] = (char)('0' + v % 10);
		v /= 10;
	}
	while (i)
		*p++ = tmp[--i];
	return p;
}

static char *put_sdec(char *p, int32_t v)
{
	if (v < 0) {
		*p++ = '-';
		return put_dec(p, (uint32_t)(-(int64_t)v));
	}
	return put_dec(p, (uint32_t)v);
}

static char *put_hex32(char *p, uint32_t v)
{
	static const char hex[] = "0123456789abcdef";
	*p++ = '0';
	*p++ = 'x';
	for (int shift = 28; shift >= 0; shift -= 4)
		*p++ = hex[(v >> shift) & 0xfu];
	return p;
}

#if defined(CONFIG_OVE_LINUX_NET)
static char *put_ipv4(char *p, const ove_sockaddr_t *address)
{
	for (unsigned int i = 0; i < 4u; i++) {
		p = put_dec(p, address->addr[i]);
		if (i != 3u)
			*p++ = '.';
	}
	return p;
}
#endif

static uint32_t uptime_ms(void)
{
	uint64_t us = 0;
	(void)ove_time_get_us(&us);
	return (uint32_t)(us / 1000u);
}

#if defined(CONFIG_OVE_LINUX_NET)
/*
 * Run after phase 1 and allow a bounded readiness window. RTOS network seams
 * publish the static address before their carrier, worker, and ARP paths
 * necessarily become connect-ready. A one-shot probe therefore describes a
 * scheduler race rather than transport health, especially on NuttX.
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
			char b[176];
			char *p = put_str(b, "[demo] socket smoke (post-phase1) OK <- ");
			p = put_ipv4(p, &peer);
			p = put_str(p, ":22: ");
			p = put_str(p, rb);
			p = put_str(p, " (ready after ");
			p = put_dec(p, uptime_ms() - started_ms);
			p = put_str(p, " ms, attempt ");
			p = put_dec(p, attempt);
			*p++ = ')';
			*p++ = '\n';
			*p = 0;
			ove_lxp_console_write(b);
			ove_socket_close(sk);
			return;
		}
		ove_socket_close(sk);

retry:
		if (attempt < 12)
			ove_thread_sleep_ms(250);
	}

	char b[112];
	char *p = put_str(b, "[demo] socket smoke (post-phase1): not ready after ");
	p = put_dec(p, uptime_ms() - started_ms);
	p = put_str(p, " ms, last rc=");
	p = put_sdec(p, last_rc);
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}
#endif

/* ---- the RTOS <-> Linux bridges: two oveRTOS message queues ---------------- */
struct lnx_line {
	char text[56];
};
#define N_READINGS 3

/* Phase-1 I/O uses fixed arrays instead of queues. feed_read/consume_write now run in the
 * privileged coordinator task, but the pre-staged array keeps the path allocation-free and makes
 * an empty feed unambiguously mean EOF. The native worker thread + two-way data flow are preserved. */
static struct lnx_line g_feed_lines[N_READINGS]; /* RTOS -> Linux: readings staged up front */
static volatile int g_feed_idx;			 /* feed_read cursor; == N_READINGS => EOF */
static volatile int g_feed_ready;	  /* all feed lines queued (so no premature EOF) */
static volatile int g_linux_done;	  /* the phase-1 program has exited */
static volatile int g_worker_exited;	  /* the worker thread has returned */
static char g_round_trip[N_READINGS][56]; /* what came back through Linux (for the verdict) */
static volatile int g_round_trip_n;

/* ---- the native RTOS worker thread (feeds, then consumes) ------------------ */
static ove_thread_t g_worker;
OVE_THREAD_DEFINE(g_worker_storage, 2048);

static void rtos_worker(void *arg)
{
	UNUSED(arg);

	/* RTOS -> Linux: stage all the readings up front into a plain array; feed_read (in the guest's
	 * SVC handler) serves them by index. The program cannot block waiting for input, so pre-staging
	 * also guarantees "empty == genuine EOF". */
	for (int i = 1; i <= N_READINGS; i++) {
		char *p = put_str(g_feed_lines[i - 1].text, "reading-");
		p = put_dec(p, (uint32_t)i);
		*p++ = '\n'; /* the program reads a line at a time */
		*p = 0;
		char b2[40];
		char *q = put_str(b2, "[rtos-feeder] -> Linux: reading-");
		q = put_dec(q, (uint32_t)i);
		*q++ = '\n';
		*q = 0;
		ove_lxp_console_write(b2);
	}
	g_feed_ready = 1;

	/* Linux -> RTOS: print each reply the moment consume_write records it — concurrently with the
	 * running program. consume_write publishes g_round_trip_n AFTER the text, so any count we read
	 * here has its text fully written. */
	int printed = 0;
	for (;;) {
		while (printed < g_round_trip_n) {
			char line[96];
			char *p = put_str(line, "[rtos-consumer] <- Linux (round trip #");
			p = put_dec(p, (uint32_t)(printed + 1));
			p = put_str(p, " @ ");
			p = put_dec(p, uptime_ms());
			p = put_str(p, " ms): \"");
			p = put_str(p, g_round_trip[printed]);
			p = put_str(p, "\"\n");
			*p = 0;
			ove_lxp_console_write(line);
			printed++;
		}
		if (g_linux_done && printed >= g_round_trip_n)
			break;
		/* Poll coarsely (not a tight spin): the worker must stay fully idle across the whole load
		 * window so it neither preempts nor churns the scheduler while demo_body reads the program
		 * image from the QUADSPI (see the OVE_PRIO_LOW note above). 50 ms comfortably spans the load;
		 * the replies then print in a small burst — correct, just not one-at-a-time. */
		ove_time_delay_ms(50);
	}
	g_worker_exited = 1;
}

/* ---- phase 1 callbacks: round-trip through the oveRTOS queues --------------- */
/* stdin: hand the program the next RTOS-produced line; empty queue == EOF (the
 * feeder pre-filled it, so "empty" really does mean the readings are exhausted). */
static long feed_read(void *ctx, int fd, void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	if (g_feed_idx >= N_READINGS)
		return 0; /* EOF: every staged reading has been served */
	const char *src = g_feed_lines[g_feed_idx++].text;
	size_t l = strlen(src);
	if (l > len)
		l = len;
	memcpy(buf, src, l);
	return (long)l;
}

/* stdout: push each line the program emits to the RTOS consumer (ISR-safe). */
static long consume_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
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

static void on_enosys(long nr)
{
	/* Prefix + UINT32_MAX + newline + terminator. */
	char b[48];
	char *p = put_str(b, "[demo] unimplemented syscall nr=");
	p = put_dec(p, (uint32_t)nr);
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}

static const char *exit_reason_name(uint8_t reason)
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
	default:
		return "unspecified";
	}
}

/* Development-target attribution for contained guest failures. Normal exits stay
 * silent; abnormal records are emitted from coordinator task context, never from
 * an exception handler, and remain bounded to one short UART line. */
static void on_guest_exit(const ove_lxp_guest_exit_info_t *info)
{
	if (!info || info->reason == OVE_LXP_EXIT_REASON_NORMAL)
		return;
	char b[192];
	char *p = put_str(b, "[lxp] guest-exit slot=");
	p = put_dec(p, (uint32_t)info->slot);
	p = put_str(p, " pid=");
	p = put_dec(p, (uint32_t)info->pid);
	p = put_str(p, " comm=");
	p = put_str(p, info->comm ? info->comm : "?");
	p = put_str(p, " status=");
	p = put_dec(p, (uint32_t)info->status);
	p = put_str(p, " reason=");
	p = put_str(p, exit_reason_name(info->reason));
	if (info->signal) {
		p = put_str(p, " signal=");
		p = put_dec(p, info->signal);
	}
	if (info->detail) {
		p = put_str(p, " detail=");
		p = put_hex32(p, info->detail);
	}
	if (info->address) {
		p = put_str(p, " address=");
		p = put_hex32(p, (uint32_t)info->address);
	}
	*p++ = '\n';
	*p = 0;
	ove_lxp_console_write(b);
}

/* ---- host (owns the index for the board-selected Buildroot CPIO backing) --- */
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
/* Deferred syscalls execute on this coordinator stack. The O0 QEMU integration build reaches
 * 4320 bytes while loading and running BusyBox, so retain nearly another full call-chain of
 * margin. OVE_THREAD_DEFINE_HOST keeps this privileged coordinator's stack in
 * backend-safe memory while a protected guest address space is active. */
OVE_THREAD_DEFINE_HOST(g_demo_storage, 8192);

static void demo_body(void *arg)
{
	UNUSED(arg);
	(void)ove_lxp_console_init(); /* bring up the program console before any I/O */
	ove_lxp_console_write("=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");
	/* First line out of the box: identifies the running image against the ELF on
	 * disk, so a stale target cannot be debugged with the wrong symbols. */
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
		char b[64];
		char *p = put_str(b, "[demo] FAIL: Linux host init failed rc=");
		p = put_sdec(p, host_rc);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(b);
		demo_exit(1);
	}

#if defined(CONFIG_OVE_LINUX_NET)
	/* Report the host-owned native interface without exposing its handle. */
	{
		ove_sockaddr_t ip = {0}, gw = {0}, nm = {0};
		if (ove_lxp_host_netif_get_addr(&g_linux_host, &ip, &gw, &nm) == OVE_OK) {
			char b[96];
			char *p = put_str(b, "[demo] eth0 up ip=");
			p = put_ipv4(p, &ip);
			p = put_str(p, " gw=");
			p = put_ipv4(p, &gw);
			*p++ = '\n';
			*p = 0;
			ove_lxp_console_write(b);
		} else {
			ove_lxp_console_write("[demo] eth0 address unavailable after bring-up\n");
		}
	}
#endif

	/* ---- Phase 1: rootfs-owned bidirectional round trip ------------------- */
	ove_lxp_console_write("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	/* BELOW the demo task: the worker feeds the readings (before the program launches) and
	 * drains its output (during/after the run). It runs when demo_body blocks — in
	 * the pre-feed wait just below and, once the program is running, in the event-driven
	 * coordinator's event_wait — so both directions co-run without the worker preempting the
	 * coordinator. (The loader's QUADSPI-NOR reads are preemption-safe in their own right — the
	 * coordinator reads the NOR through a non-cacheable bounded MPU region, see
	 * rootfs_window callback — so this priority is about I/O ordering, not protecting the load.) */
	if (ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", rtos_worker, NULL,
			    OVE_PRIO_LOW, sizeof(g_worker_storage_stack),
			    g_worker_storage_stack) != OVE_OK) {
		ove_lxp_console_write("[demo] FAIL: ove_thread_init\n");
		demo_exit(1);
	}
	while (!g_feed_ready) /* let the feeder fill the queue before the program reads */
		ove_thread_sleep_ms(1); /* BLOCKING sleep so the lower-priority worker runs: NuttX's
					 * ove_time_delay_ms(1) busy-waits sub-tick (10 ms tick) and never
					 * yields → the OVE_PRIO_LOW worker starves and the demo hangs here
					 * before phase 2.  ove_thread_sleep_ms always usleep()s. */

	const ove_lxp_launch_config_t cfg1 = {
		.write_fn = consume_write,
		.read_fn = feed_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
		.rt_scope_read = linux_rt_scope_proc_read,
	};
	ove_lxp_console_write("[demo] launching the Linux guest round-trip mode...\n");
	int rc1 = run_guest_mode(&cfg1, "roundtrip");

	g_linux_done = 1;
	while (!g_worker_exited) /* wait for the worker to drain and return */
		ove_thread_sleep_ms(1); /* blocking — must yield to the lower-priority worker (see above) */
	(void)ove_thread_deinit(g_worker);

	int ok = (rc1 >= 0) && (g_round_trip_n == N_READINGS);
	for (int i = 0; ok && i < N_READINGS; i++) {
		char want[16];
		char *p = put_str(want, "reading-");
		p = put_dec(p, (uint32_t)(i + 1));
		*p = 0;
		ok = (strcmp(g_round_trip[i], want) == 0);
	}
	if (!ok) {
		char b[112];
		char *p = put_str(b, "[demo] FAIL: phase-1 round trip mismatch rc=");
		p = put_sdec(p, rc1);
		p = put_str(p, " received=");
		p = put_dec(p, (uint32_t)g_round_trip_n);
		*p++ = '\n';
		*p = 0;
		ove_lxp_console_write(b);
		demo_exit(1);
	}
	ove_lxp_console_write(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");

#if defined(CONFIG_OVE_LINUX_NET)
	/* A real post-readiness TCP round trip over the same host transport used by
	 * personality sockets and netfs. */
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
		.on_enosys = on_enosys,
		.on_guest_exit = on_guest_exit,
		.rt_scope_read = linux_rt_scope_proc_read,
	};
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
	linux_interop_qualification_report(
		&observation, audit_threads, sizeof(audit_threads) / sizeof(audit_threads[0]));
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
