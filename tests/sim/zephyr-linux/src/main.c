/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Zephyr Linux-personality on-target test (mps2/an521/cpu0, Cortex-M33): boot a
 * real minimal Buildroot rootfs and run an interactive shell. This file is now a
 * thin test driver — the engine seam (svc trap, NOMMU process model, MPU
 * domains, run loop) lives in the reusable backends/zephyr/zephyr_lnx.c module
 * (ove_lnx_zephyr_run); here we only supply the host pieces:
 *   - the rootfs: an embedded newc CPIO (a Buildroot rootfs.cpio: BusyBox 1.38 +
 *     /etc + /dev + applet symlinks), parsed by ove_lnx_cpio_to_rootfs;
 *   - console I/O: ARM semihosting for output, a scripted keystroke buffer for
 *     input (the tty line discipline / ^C uses the seam's tty helpers);
 *   - the assertion: the captured session must match EXPECT_MSG.
 *
 * The shell runs commands from stdin: it writes /tmp/foo with a `>` redirect and
 * appends with `>>` (into a writable in-memory tmpfs overlaid on the read-only
 * CPIO rootfs), then reads it back with cat. This boots stock upstream userspace
 * from a stock rootfs image, unprivileged, on a NOMMU MCU.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "ove/linux/syscall.h"
#include "ove/linux/zephyr.h"

#include "loader_rootfs_image.h" /* ove_test_rootfs_cpio[], _len (a real Buildroot rootfs) */

/* ARM semihosting (host console + clean QEMU exit). */
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

static void sh_write_hex(const char *tag, uint32_t v)
{
	static const char hx[] = "0123456789abcdef";
	char b[12];
	b[0] = ' ';
	for (int i = 0; i < 8; i++)
		b[1 + i] = hx[(v >> (28 - 4 * i)) & 0xf];
	b[9] = '\n';
	b[10] = 0;
	sh_write0(tag);
	sh_write0(b);
}

static void sh_exit(unsigned int code)
{
	unsigned long block[2] = {0x20026u /* ADP_Stopped_ApplicationExit */, code};
	semihost(0x20 /* SYS_EXIT_EXTENDED */, block);
	for (;;) {
	}
}

/* ---- console I/O (the seam calls these as the program's fd 0/1/2) ---------- */
static char g_cap[512];
static volatile size_t g_cap_len;

static long capture_write(void *ctx, int fd, const void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	if (g_cap_len + len > sizeof(g_cap))
		len = sizeof(g_cap) - g_cap_len;
	memcpy(g_cap + g_cap_len, buf, len);
	g_cap_len += len;
	return (long)len;
}

/* Scripted stdin (the engine is the terminal; deterministic, no live-TTY flake).
 * Writes a file into the writable tmpfs overlay (shell `>` redirect -> open(O_CREAT)
 * + write), appends to it (`>>` -> O_APPEND), then reads it back with cat. */
static const char g_input[] = "echo hello > /tmp/foo\necho world >> /tmp/foo\ncat /tmp/foo\nexit\n";
static volatile size_t g_input_pos;

static long console_read(void *ctx, int fd, void *buf, size_t len)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(fd);
	/* Feed scripted keystrokes (hush is in raw mode and echoes them itself, so
	 * the engine does not echo). One line per block read. A ^C in ISIG mode is
	 * the tty interrupt key: latch SIGINT for delivery at the syscall boundary
	 * and interrupt the read (EINTR), rather than return it as a byte. */
	char *out = (char *)buf;
	size_t n = 0;
	while (n < len && g_input_pos < sizeof(g_input) - 1) {
		char c = g_input[g_input_pos];
		if (c == 0x03 && ove_lnx_zephyr_tty_isig()) {
			if (n > 0)
				break; /* deliver buffered input first; ^C next read */
			g_input_pos++; /* consume the ^C */
			ove_lnx_zephyr_post_signal(OVE_LNX_SIGINT);
			return -OVE_LNX_EINTR;
		}
		g_input_pos++;
		out[n++] = c;
		if (c == '\n')
			break;
	}
	return (long)n; /* 0 = EOF */
}

/* Optional bring-up diagnostic: report a syscall the personality lacks. */
static void on_enosys(long nr)
{
	sh_write_hex("ENOSYS nr", (uint32_t)nr);
}

/* ---- rootfs (parsed from the embedded Buildroot CPIO) ---------------------- */
#define ROOTFS_MAX_FILES 256
static ove_lnx_file_t g_rootfs[ROOTFS_MAX_FILES];
static char g_rootfs_names[8192];

#define EXPECT_MSG                                                \
	"/ # echo hello > /tmp/foo\n/ # echo world >> /tmp/foo\n" \
	"/ # cat /tmp/foo\nhello\nworld\n/ # exit\n"

int main(void)
{
	sh_write0("=== Zephyr Linux personality: Buildroot rootfs boot (an521) ===\n");

	int rootfs_n = ove_lnx_cpio_to_rootfs(ove_test_rootfs_cpio, ove_test_rootfs_cpio_len,
					      g_rootfs, ROOTFS_MAX_FILES, g_rootfs_names,
					      sizeof(g_rootfs_names));
	if (rootfs_n <= 0) {
		sh_write0("[zephyr-linux] FAIL: rootfs CPIO parse failed\n");
		sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
		sh_exit(1);
	}

	/* Run /bin/busybox as the "sh" init process via the engine seam. */
	const ove_lnx_zephyr_config_t cfg = {
		.rootfs = g_rootfs,
		.rootfs_count = rootfs_n,
		.write_fn = capture_write,
		.read_fn = console_read,
		.io_ctx = NULL,
		.on_enosys = on_enosys,
	};
	const char *const sh_argv[] = {"sh", NULL};
	int rc = ove_lnx_zephyr_run(&cfg, "/bin/busybox", 1, sh_argv);

	if (rc >= 0 && g_cap_len == sizeof(EXPECT_MSG) - 1 &&
	    memcmp(g_cap, EXPECT_MSG, g_cap_len) == 0) {
		sh_write0(
			"[zephyr-linux] booted a Buildroot rootfs -> busybox /bin/sh; wrote + "
			"appended /tmp/foo in the writable tmpfs overlay, cat'd it back, exit OK\n");
		sh_write0("\n=== Summary: 0 test group(s) had failures ===\n");
		sh_exit(0);
	}

	sh_write_hex("[zephyr-linux] FAIL: run rc", (uint32_t)rc);
	sh_write0("  out=");
	if (g_cap_len) {
		g_cap[g_cap_len < sizeof(g_cap) ? g_cap_len : sizeof(g_cap) - 1] = 0;
		sh_write0(g_cap);
	}
	sh_write0("\n=== Summary: 1 test group(s) had failures ===\n");
	sh_exit(1);
	return 0;
}
