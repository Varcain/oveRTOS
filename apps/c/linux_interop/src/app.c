/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * RTOS-kernel <-> Linux-personality interop demo (mps2/an521, Cortex-M33).
 *
 * One firmware image runs two worlds side by side:
 *   - a NATIVE RTOS thread (a privileged Zephyr kernel thread) that blocks on a
 *     kernel message queue and processes whatever it receives, in real time;
 *   - a stock LINUX program (BusyBox, an unprivileged uClibc bFLT) launched
 *     through the Linux personality (ove_lnx_zephyr_run) from a real Buildroot
 *     rootfs.
 *
 * They INTEROP across the personality boundary: the Linux program's stdout is
 * fed — by the host write callback, straight from the svc trap — into the RTOS
 * kernel's k_msgq; the native RTOS thread drains that queue and acts on each
 * line. So a Linux process and an RTOS task exchange data through an ordinary
 * RTOS kernel object, in one image, concurrently.
 *
 * The boundary callback (demo_write) runs in the svc-trap (exception) context,
 * so it only does k_msgq_put(K_NO_WAIT) (ISR-safe); the worker blocks with
 * k_msgq_get in thread context. Output is via ARM semihosting (matches the
 * personality test; deterministic under QEMU).
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

/* ---- the RTOS <-> Linux bridge: a native kernel message queue -------------- */
struct lnx_line {
	char text[56];
};
K_MSGQ_DEFINE(g_to_rtos, sizeof(struct lnx_line), 8, 4);

static volatile int g_linux_done; /* set when the Linux program has exited */
static char g_consumed[4][56];	  /* what the worker received (for the verdict) */
static volatile int g_consumed_n;

/* ---- the native RTOS worker thread ----------------------------------------- */
#define WORKER_STACK 2048
#define WORKER_PRIO 4 /* preempts the unprivileged user threads (prio 5) to consume promptly */
K_THREAD_STACK_DEFINE(g_worker_stack, WORKER_STACK);
static struct k_thread g_worker;

static void rtos_worker(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	sh_write0(
		"[rtos-worker] native Zephyr thread up; blocking on the kernel msgq for Linux data\n");

	struct lnx_line m;
	for (;;) {
		/* Drain the queue the Linux side feeds; time out periodically to notice
		 * the Linux program has exited and there is nothing left to consume. */
		if (k_msgq_get(&g_to_rtos, &m, K_MSEC(50)) == 0) {
			if (g_consumed_n < (int)(sizeof(g_consumed) / sizeof(g_consumed[0]))) {
				strncpy(g_consumed[g_consumed_n], m.text,
					sizeof(g_consumed[0]) - 1);
				g_consumed[g_consumed_n][sizeof(g_consumed[0]) - 1] = 0;
			}
			g_consumed_n++;
			char line[112];
			char *p = put_str(line, "[rtos-worker] consumed from Linux #");
			p = put_dec(p, (uint32_t)g_consumed_n);
			p = put_str(p, " @ ");
			p = put_dec(p, (uint32_t)k_uptime_get());
			p = put_str(p, " ms: \"");
			p = put_str(p, m.text);
			p = put_str(p, "\"\n");
			*p = 0;
			sh_write0(line);
		} else if (g_linux_done) {
			break; /* Linux finished and the queue is drained */
		}
	}
	sh_write0("[rtos-worker] Linux side finished; worker exiting\n");
}

/* ---- the Linux personality's console callbacks ----------------------------- */
/* stdout: runs in the svc-trap context, so only ISR-safe work — hand each line
 * to the RTOS worker via the kernel msgq (K_NO_WAIT; drop if the worker is slow). */
static long demo_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	struct lnx_line m;
	size_t n = len < sizeof(m.text) - 1 ? len : sizeof(m.text) - 1;
	memcpy(m.text, buf, n);
	while (n && (m.text[n - 1] == '\n' || m.text[n - 1] == '\r'))
		n--; /* trim the line ending for a clean log */
	m.text[n] = 0;
	if (n)
		k_msgq_put(&g_to_rtos, &m, K_NO_WAIT);
	return (long)len; /* the program sees a full write regardless */
}

static long demo_read(void *ctx, int fd, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return 0; /* the program has no stdin (it is `sh -c <script>`) */
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

/* The Linux program emits three "measurements" the RTOS worker consumes. */
static const char *const g_expect[] = {"measurement-1", "measurement-2", "measurement-3"};

int main(void)
{
	sh_write0(
		"=== oveRTOS demo: a native RTOS thread + a Linux program, side by side (an521) ===\n");

	int rootfs_n = ove_lnx_cpio_to_rootfs(ove_test_rootfs_cpio, ove_test_rootfs_cpio_len,
					      g_rootfs, ROOTFS_MAX_FILES, g_rootfs_names,
					      sizeof(g_rootfs_names));
	if (rootfs_n <= 0) {
		sh_write0("[demo] FAIL: rootfs CPIO parse failed\n");
		sh_exit(1);
	}

	/* Start the native RTOS worker; it runs concurrently with the Linux program. */
	k_thread_create(&g_worker, g_worker_stack, WORKER_STACK, rtos_worker, NULL, NULL, NULL,
			WORKER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&g_worker, "rtos-worker");

	/* Launch the Linux program through the personality: BusyBox sh runs a small
	 * script that prints three measurements (each echo -> stdout -> the worker). */
	const ove_lnx_zephyr_config_t cfg = {
		.rootfs = g_rootfs,
		.rootfs_count = rootfs_n,
		.write_fn = demo_write,
		.read_fn = demo_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const sh_argv[] = {
		"sh", "-c", "echo measurement-1; echo measurement-2; echo measurement-3", NULL};
	sh_write0("[demo] launching the Linux program (BusyBox sh) via the personality...\n");
	int rc = ove_lnx_zephyr_run(&cfg, "/bin/busybox", 3, sh_argv);

	/* The Linux program has exited; let the worker drain and stop, then verify. */
	g_linux_done = 1;
	k_thread_join(&g_worker, K_FOREVER);

	int ok = (rc >= 0) && (g_consumed_n == 3);
	for (int i = 0; ok && i < 3; i++)
		ok = (strcmp(g_consumed[i], g_expect[i]) == 0);

	if (ok) {
		sh_write0(
			"[demo] the RTOS worker received all 3 measurements from the Linux process.\n");
		sh_write0(
			"=== interop demo OK: RTOS kernel + Linux personality ran concurrently ===\n");
		sh_exit(0);
	}
	sh_write0("[demo] FAIL: interop mismatch (rc or consumed count/text)\n");
	sh_exit(1);
	return 0;
}
