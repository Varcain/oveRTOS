/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo (mps2/an521, Cortex-M33).
 *
 * One firmware image, two worlds, two phases:
 *
 *  Phase 1 — BIDIRECTIONAL round trip. A native RTOS thread (a privileged Zephyr
 *  kernel thread) feeds three "sensor readings" INTO a stock Linux program
 *  (BusyBox `cat`, an unprivileged uClibc bFLT) through the program's stdin, and
 *  drains what `cat` echoes back OUT of its stdout — both halves crossing the
 *  personality boundary through ordinary RTOS kernel message queues:
 *      RTOS feeder -> g_feed_q -> read cb -> [Linux cat] -> write cb -> g_consume_q -> RTOS consumer
 *  So an RTOS task and a Linux process exchange data in both directions.
 *
 *  Phase 2 — INTERACTIVE shell. The program then drops into an interactive
 *  BusyBox `sh`: the read callback returns real keystrokes (ARM semihosting
 *  SYS_READC) and the write callback echoes to the console, so the user can run
 *  commands (ls, echo, cat, pwd, ...) and `exit` to finish.
 *
 * The personality callbacks run in the svc-trap (exception) context, so they do
 * only non-blocking work there: k_msgq_put/get(K_NO_WAIT) and semihosting. The
 * RTOS worker blocks (k_msgq_get) in normal thread context. Phase 1 pre-fills
 * the feed queue before launching, so the program never sees a premature EOF.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "ove/linux/syscall.h"
#include "ove/linux/zephyr.h"

#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len — a real Buildroot rootfs */

/* ---- ARM semihosting (host console + clean QEMU exit) ---------------------- */
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

/* Minimal string builders (no libc printf dependency, like the personality test). */
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

/* ---- the RTOS <-> Linux bridges: two native kernel message queues ---------- */
struct lnx_line {
	char text[56];
};
K_MSGQ_DEFINE(g_feed_q, sizeof(struct lnx_line), 8, 4);	   /* RTOS -> Linux (program stdin) */
K_MSGQ_DEFINE(g_consume_q, sizeof(struct lnx_line), 8, 4); /* Linux -> RTOS (program stdout) */

#define N_READINGS 3
static volatile int g_feed_ready;	  /* all feed lines queued (so no premature EOF) */
static volatile int g_linux_done;	  /* the phase-1 program has exited */
static char g_round_trip[N_READINGS][56]; /* what came back through Linux (for the verdict) */
static volatile int g_round_trip_n;

/* ---- the native RTOS worker thread (feeds, then consumes) ------------------ */
#define WORKER_STACK 2048
#define WORKER_PRIO 4 /* preempts the unprivileged user threads (prio 5) */
K_THREAD_STACK_DEFINE(g_worker_stack, WORKER_STACK);
static struct k_thread g_worker;

static void rtos_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* RTOS -> Linux: produce the readings up front so they are all waiting when
	 * the program starts reading (the read callback cannot block to wait). */
	for (int i = 1; i <= N_READINGS; i++) {
		struct lnx_line m;
		char *p = put_str(m.text, "reading-");
		p = put_dec(p, (uint32_t)i);
		*p++ = '\n'; /* the program reads a line at a time */
		*p = 0;
		k_msgq_put(&g_feed_q, &m, K_NO_WAIT);
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
		if (k_msgq_get(&g_consume_q, &m, K_MSEC(50)) == 0) {
			if (g_round_trip_n < N_READINGS) {
				strncpy(g_round_trip[g_round_trip_n], m.text,
					sizeof(g_round_trip[0]) - 1);
				g_round_trip[g_round_trip_n][sizeof(g_round_trip[0]) - 1] = 0;
			}
			g_round_trip_n++;
			char line[80];
			char *p = put_str(line, "[rtos-consumer] <- Linux (round trip #");
			p = put_dec(p, (uint32_t)g_round_trip_n);
			p = put_str(p, "): \"");
			p = put_str(p, m.text);
			p = put_str(p, "\"\n");
			*p = 0;
			sh_write0(line);
		} else if (g_linux_done) {
			break;
		}
	}
}

/* ---- phase 1 callbacks: round-trip through the RTOS message queues ---------- */
/* stdin: hand the program the next RTOS-produced line; empty queue == EOF (the
 * feeder pre-filled it, so "empty" really does mean the readings are exhausted). */
static long feed_read(void *ctx, int fd, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	struct lnx_line m;
	if (k_msgq_get(&g_feed_q, &m, K_NO_WAIT) != 0)
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
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	struct lnx_line m;
	size_t n = len < sizeof(m.text) - 1 ? len : sizeof(m.text) - 1;
	memcpy(m.text, buf, n);
	while (n && (m.text[n - 1] == '\n' || m.text[n - 1] == '\r'))
		n--;
	m.text[n] = 0;
	if (n)
		k_msgq_put(&g_consume_q, &m, K_NO_WAIT);
	return (long)len;
}

/* ---- phase 2 callbacks: a live console for the interactive shell ------------ */
/* stdin: one real keystroke at a time (the shell's line editor reads char-by-char). */
static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
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

/* stdout: echo to the host console; translate \n -> \r\n so it reads cleanly
 * regardless of the host terminal mode. */
static long console_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
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

int main(void)
{
	sh_write0(
		"=== oveRTOS demo: a native RTOS thread + a Linux program, two-way (an521) ===\n");

	g_rootfs_n = ove_lnx_cpio_to_rootfs(ove_test_rootfs_cpio, ove_test_rootfs_cpio_len,
					    g_rootfs, ROOTFS_MAX_FILES, g_rootfs_names,
					    sizeof(g_rootfs_names));
	if (g_rootfs_n <= 0) {
		sh_write0("[demo] FAIL: rootfs CPIO parse failed\n");
		sh_exit(1);
	}

	/* ---- Phase 1: bidirectional round trip through BusyBox `cat` ---------- */
	sh_write0("\n-- phase 1: RTOS thread <-> Linux program (bidirectional) --\n");
	k_thread_create(&g_worker, g_worker_stack, WORKER_STACK, rtos_worker, NULL, NULL, NULL,
			WORKER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&g_worker, "rtos-worker");
	while (!g_feed_ready) /* let the feeder fill the queue before the program reads */
		k_msleep(1);

	const ove_lnx_zephyr_config_t cfg1 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = consume_write,
		.read_fn = feed_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const cat_argv[] = {"cat", NULL}; /* reads stdin -> writes stdout */
	sh_write0("[demo] launching the Linux program (BusyBox cat) to relay the readings...\n");
	int rc1 = ove_lnx_zephyr_run(&cfg1, "/bin/busybox", 1, cat_argv);
	g_linux_done = 1;
	k_thread_join(&g_worker, K_FOREVER);

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
	const ove_lnx_zephyr_config_t cfg2 = {
		.rootfs = g_rootfs,
		.rootfs_count = g_rootfs_n,
		.write_fn = console_write,
		.read_fn = console_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const sh_argv[] = {"sh", NULL};
	int rc2 = ove_lnx_zephyr_run(&cfg2, "/bin/busybox", 1, sh_argv);

	sh_write0("\n=== interop demo done (interactive shell exited) ===\n");
	sh_exit(rc2 >= 0 ? 0 : 1);
	return 0;
}
