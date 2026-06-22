/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo (mps2/an521, Cortex-M33).
 *
 * One firmware image, two worlds, two phases — built entirely on the
 * engine-agnostic oveRTOS APIs (ove_thread / ove_queue / ove_time) on the RTOS
 * side and the Linux-personality runner (ove_lnx_run) on the Linux side; no
 * direct Zephyr kernel calls.
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (ove_thread) feeds
 *  three "sensor readings" INTO a stock Linux program (BusyBox `cat`, an
 *  unprivileged uClibc bFLT) through its stdin, and drains what it echoes back
 *  OUT of its stdout — both halves crossing the personality boundary through
 *  oveRTOS message queues (ove_queue):
 *      RTOS feeder -> g_feed_q -> read cb -> [Linux cat] -> write cb -> g_consume_q -> RTOS consumer
 *
 *  Phase 2 — INTERACTIVE shell. The program then drops into an interactive
 *  BusyBox `sh`; type commands (ls /, echo hi, cat /etc/hostname, ...) and
 *  `exit` to finish.
 *
 * The personality's I/O callbacks run in the svc-trap (exception) context, so
 * there they use only the ISR-safe queue variants
 * (ove_queue_send_from_isr / ove_queue_receive_from_isr) and ARM semihosting
 * (the console transport — an architecture facility, not an RTOS primitive). The
 * RTOS worker uses the blocking ove_queue_send / ove_queue_receive in thread
 * context. Phase 1 pre-fills the feed queue before launching, so the program
 * never sees a premature EOF.
 */

#include <string.h>

#include "ove/queue.h"
#include "ove/thread.h"
#include "ove/time.h"

#include "ove/app.h"
#include "ove/linux/run.h"
#include "ove/linux/syscall.h"

#include "ove_config.h" /* CONFIG_OVE_RTOS_FREERTOS — selects the app lifecycle below */

#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len — a real Buildroot rootfs */

#define UNUSED(x) ((void)(x))

/* ---- ARM semihosting (console transport + clean QEMU exit) ----------------- */
static long semihost(unsigned long op, void *arg)
{
	register unsigned long r0 __asm__("r0") = op;
	register void *r1 __asm__("r1") = arg;
	__asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
	return (long)r0;
}

static void sh_write0(const char *s)
{
	semihost(0x04 /* SYS_WRITE0 */, (void *)s);
}

static void sh_writec(char c)
{
	semihost(0x03 /* SYS_WRITEC */, &c);
}

static int sh_readc(void)
{
	return (int)semihost(0x07 /* SYS_READC */, NULL);
}

static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}

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

static uint32_t uptime_ms(void)
{
	uint64_t us = 0;
	(void)ove_time_get_us(&us);
	return (uint32_t)(us / 1000u);
}

/* ---- the RTOS <-> Linux bridges: two oveRTOS message queues ---------------- */
struct lnx_line {
	char text[56];
};
#define QDEPTH 8
static ove_queue_t g_feed_q;	/* RTOS -> Linux (program stdin)  */
static ove_queue_t g_consume_q; /* Linux -> RTOS (program stdout) */
static ove_queue_storage_t g_feed_storage, g_consume_storage;
static uint8_t g_feed_buf[sizeof(struct lnx_line) * QDEPTH];
static uint8_t g_consume_buf[sizeof(struct lnx_line) * QDEPTH];

#define N_READINGS 3
static volatile int g_feed_ready;	  /* all feed lines queued (so no premature EOF) */
static volatile int g_linux_done;	  /* the phase-1 program has exited */
static volatile int g_worker_exited;	  /* the worker thread has returned */
static char g_round_trip[N_READINGS][56]; /* what came back through Linux (for the verdict) */
static volatile int g_round_trip_n;

/* ---- the native RTOS worker thread (feeds, then consumes) ------------------ */
static ove_thread_t g_worker;
static ove_thread_storage_t g_worker_storage;
static uint8_t g_worker_stack[2048] __attribute__((aligned(8)));

static void rtos_worker(void *arg)
{
	UNUSED(arg);

	/* RTOS -> Linux: produce the readings up front so they are all waiting when
	 * the program starts reading (the read callback cannot block to wait). */
	for (int i = 1; i <= N_READINGS; i++) {
		struct lnx_line m;
		char *p = put_str(m.text, "reading-");
		p = put_dec(p, (uint32_t)i);
		*p++ = '\n'; /* the program reads a line at a time */
		*p = 0;
		(void)ove_queue_send(g_feed_q, &m, OVE_MS(100));
		char b2[40];
		char *q = put_str(b2, "[rtos-feeder] -> Linux: reading-");
		q = put_dec(q, (uint32_t)i);
		*q++ = '\n';
		*q = 0;
		sh_write0(b2);
	}
	g_feed_ready = 1;

	/* Linux -> RTOS: drain what the program echoes back, concurrently with it. */
	struct lnx_line m;
	for (;;) {
		if (ove_queue_receive(g_consume_q, &m, OVE_MS(50)) == OVE_OK) {
			if (g_round_trip_n < N_READINGS) {
				strncpy(g_round_trip[g_round_trip_n], m.text,
					sizeof(g_round_trip[0]) - 1);
				g_round_trip[g_round_trip_n][sizeof(g_round_trip[0]) - 1] = 0;
			}
			g_round_trip_n++;
			char line[96];
			char *p = put_str(line, "[rtos-consumer] <- Linux (round trip #");
			p = put_dec(p, (uint32_t)g_round_trip_n);
			p = put_str(p, " @ ");
			p = put_dec(p, uptime_ms());
			p = put_str(p, " ms): \"");
			p = put_str(p, m.text);
			p = put_str(p, "\"\n");
			*p = 0;
			sh_write0(line);
		} else if (g_linux_done) {
			break;
		}
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
	struct lnx_line m;
	if (ove_queue_receive_from_isr(g_feed_q, &m) != OVE_OK)
		return 0; /* EOF */
	size_t l = strlen(m.text);
	if (l > len)
		l = len;
	memcpy(buf, m.text, l);
	return (long)l;
}

/* stdout: push each line the program emits to the RTOS consumer (ISR-safe). */
static long consume_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	struct lnx_line m;
	size_t n = len < sizeof(m.text) - 1 ? len : sizeof(m.text) - 1;
	memcpy(m.text, buf, n);
	while (n && (m.text[n - 1] == '\n' || m.text[n - 1] == '\r'))
		n--;
	m.text[n] = 0;
	if (n)
		(void)ove_queue_send_from_isr(g_consume_q, &m);
	return (long)len;
}

/* ---- phase 2 callbacks: a live console for the interactive shell ------------ */
/* stdin: one real keystroke at a time (the shell's line editor reads char-by-char). */
static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	if (len == 0)
		return 0;
	int c = sh_readc();
	if (c < 0)
		return 0; /* host EOF -> the shell exits */
	if (c == '\n')
		c = '\r'; /* normalize Enter to CR: the shell's raw line editor expects it */
	*(char *)buf = (char)c;
	return 1;
}

/* stdout: echo to the host console; translate \n -> \r\n so it reads cleanly. */
static long console_write(void *ctx, int fd, const void *buf, size_t len)
{
	UNUSED(ctx);
	UNUSED(fd);
	const char *p = (const char *)buf;
	for (size_t i = 0; i < len; i++) {
		if (p[i] == '\n')
			sh_writec('\r');
		sh_writec(p[i]);
	}
	return (long)len;
}

static void on_enosys(long nr)
{
	char b[40];
	char *p = put_str(b, "[demo] unimplemented syscall nr=");
	p = put_dec(p, (uint32_t)nr);
	*p++ = '\n';
	*p = 0;
	sh_write0(b);
}

/* ---- rootfs (parsed from the embedded Buildroot CPIO) ---------------------- */
#define ROOTFS_MAX_FILES 256
static ove_lnx_file_t g_rootfs[ROOTFS_MAX_FILES];
static char g_rootfs_names[8192];
static int g_rootfs_n;

/* The engine-agnostic demo. On FreeRTOS the scheduler starts inside ove_run(), so
 * this must run in a task; on Zephyr ove_main() is already a running thread and
 * calls it inline. ove_main() (below) wires the per-engine lifecycle. */
#ifdef CONFIG_OVE_RTOS_FREERTOS
static ove_thread_t g_demo;
static ove_thread_storage_t g_demo_storage;
static uint8_t g_demo_stack[4096] __attribute__((aligned(8)));
#endif

static void demo_body(void *arg)
{
	UNUSED(arg);
	sh_write0("=== oveRTOS demo: a native RTOS thread + a Linux program, two-way ===\n");

	g_rootfs_n = ove_lnx_cpio_to_rootfs(ove_test_rootfs_cpio, ove_test_rootfs_cpio_len,
					    g_rootfs, ROOTFS_MAX_FILES, g_rootfs_names,
					    sizeof(g_rootfs_names));
	if (g_rootfs_n <= 0) {
		sh_write0("[demo] FAIL: rootfs CPIO parse failed\n");
		sh_exit(1);
	}

	/* RTOS-side primitives, all via oveRTOS APIs. */
	if (ove_queue_init(&g_feed_q, &g_feed_storage, g_feed_buf, sizeof(struct lnx_line),
			   QDEPTH) != OVE_OK ||
	    ove_queue_init(&g_consume_q, &g_consume_storage, g_consume_buf, sizeof(struct lnx_line),
			   QDEPTH) != OVE_OK) {
		sh_write0("[demo] FAIL: ove_queue_init\n");
		sh_exit(1);
	}

	/* ---- Phase 1: bidirectional round trip through BusyBox `cat` ---------- */
	sh_write0("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	if (ove_thread_init(&g_worker, &g_worker_storage, "rtos-worker", rtos_worker, NULL,
			    OVE_PRIO_HIGH, sizeof(g_worker_stack), g_worker_stack) != OVE_OK) {
		sh_write0("[demo] FAIL: ove_thread_init\n");
		sh_exit(1);
	}
	while (!g_feed_ready) /* let the feeder fill the queue before the program reads */
		ove_time_delay_ms(1);

	const ove_lnx_run_config_t cfg1 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = consume_write,
		.read_fn = feed_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const cat_argv[] = {"cat", NULL}; /* reads stdin -> writes stdout */
	sh_write0("[demo] launching the Linux program (BusyBox cat) to relay the readings...\n");
	int rc1 = ove_lnx_run(&cfg1, "/bin/busybox", 1, cat_argv);

	g_linux_done = 1;
	while (!g_worker_exited) /* wait for the worker to drain and return */
		ove_time_delay_ms(1);
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
		sh_write0("[demo] FAIL: phase-1 round trip mismatch\n");
		sh_exit(1);
	}
	sh_write0(
		"[demo] phase 1 OK: 3 readings made the full RTOS -> Linux -> RTOS round trip.\n");

	/* ---- Phase 2: drop into an interactive shell -------------------------- */
	sh_write0("\n-- phase 2: interactive BusyBox shell (type commands; `exit` to quit) --\n");
	const ove_lnx_run_config_t cfg2 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = console_write,
		.read_fn = console_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const sh_argv[] = {"sh", NULL};
	int rc2 = ove_lnx_run(&cfg2, "/bin/busybox", 1, sh_argv);

	sh_write0("\n=== interop demo done (interactive shell exited) ===\n");
	sh_exit(rc2 >= 0 ? 0 : 1);
}

void ove_main(void)
{
#ifdef CONFIG_OVE_RTOS_FREERTOS
	/* FreeRTOS: the scheduler starts in ove_run(); run the demo in a task. */
	if (ove_thread_init(&g_demo, &g_demo_storage, "demo", demo_body, NULL, OVE_PRIO_NORMAL,
			    sizeof(g_demo_stack), g_demo_stack) != OVE_OK) {
		sh_write0("[demo] FAIL: demo thread init\n");
		sh_exit(1);
	}
	ove_run(); /* ove_thread_start_scheduler() — never returns */
#else
	/* Zephyr: ove_main() already runs as a thread with the scheduler up. */
	demo_body(NULL);
#endif
}
