/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality /dev device-layer tests: register a mock character device
 * and drive lxp_syscall() (no hardware SVC, no run loop) to check the
 * FD_DEV routing — open/read/write/ioctl/poll/lseek/close, stat + getdents,
 * fd refcounting across dup, and the deferred (block + coordinator-retry) path.
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "lxp/lxp_dev.h"
#include "lxp/lxp_syscall.h"

#include <stdint.h>
#include <string.h>

/* access_ok — non-static in ove_linux_syscall.c so a device ioctl handler can
 * validate its user pointer (the confused-deputy guard). */
int user_ok(const lxp_proc_t *p, const void *ptr, size_t len, int write);

/* ---- a mock character device ----------------------------------------------- */
#define MOCK_IOC_GET 0x1001ul /* read g_mock_val into *arg */
#define MOCK_IOC_SET 0x1002ul /* write *arg into g_mock_val */

static uint8_t g_mock_rbuf[64]; /* bytes a read() returns */
static size_t g_mock_rlen;
static uint8_t g_mock_wbuf[64]; /* bytes the last write() captured */
static size_t g_mock_wlen;
static int g_mock_block;    /* while set, read() returns -EAGAIN (would block) */
static int g_mock_released; /* count of ops->release calls */
static int g_mock_opened;   /* count of ops->open calls */
static uint32_t g_mock_val; /* the MOCK_IOC_GET/SET register */

static long mock_open(struct lxp_dev *d, struct lxp_dev_open *o, int flags)
{
	(void)d;
	(void)o;
	(void)flags;
	g_mock_opened++;
	return 0;
}

static long mock_release(struct lxp_dev *d, struct lxp_dev_open *o)
{
	(void)d;
	(void)o;
	g_mock_released++;
	return 0;
}

static long mock_read(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		      void *buf, size_t len)
{
	(void)d;
	(void)o;
	(void)p;
	if (g_mock_block)
		return -LXP_EAGAIN; /* would block → the core parks the caller */
	size_t n = g_mock_rlen < len ? g_mock_rlen : len;
	memcpy(buf, g_mock_rbuf, n);
	return (long)n;
}

static long mock_write(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		       const void *buf, size_t len)
{
	(void)d;
	(void)o;
	(void)p;
	size_t n = len < sizeof(g_mock_wbuf) ? len : sizeof(g_mock_wbuf);
	memcpy(g_mock_wbuf, buf, n);
	g_mock_wlen = n;
	return (long)len;
}

static long mock_ioctl(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		       unsigned long cmd, unsigned long arg)
{
	(void)d;
	(void)o;
	if (cmd == MOCK_IOC_GET) {
		if (!user_ok(p, (void *)arg, sizeof(uint32_t), 1)) /* kernel writes *arg */
			return -LXP_EFAULT;
		*(uint32_t *)arg = g_mock_val;
		return 0;
	}
	if (cmd == MOCK_IOC_SET) {
		if (!user_ok(p, (const void *)arg, sizeof(uint32_t), 0)) /* kernel reads *arg */
			return -LXP_EFAULT;
		g_mock_val = *(const uint32_t *)arg;
		return 0;
	}
	return -LXP_ENOTTY; /* unknown command */
}

static unsigned mock_poll(struct lxp_dev *d, struct lxp_dev_open *o)
{
	(void)d;
	(void)o;
	return LXP_POLLIN | LXP_POLLOUT;
}

/* mmap(2) (P3): hand back a fixed "device buffer" + a cache-attr hint, rejecting a
 * request past the device extent — the shape the /dev/fb0 driver's op has. */
static uint8_t g_mock_fb[256];
static long mock_mmap(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		      size_t len, uint32_t pgoff, uintptr_t *phys, unsigned *attrs)
{
	(void)o;
	(void)p;
	if (pgoff != 0 || len > d->size)
		return -LXP_EINVAL;
	*phys = (uintptr_t)g_mock_fb;
	*attrs = LXP_MAP_NC;
	return 0;
}

static const struct lxp_dev_ops mock_ops = {
	.open = mock_open,
	.release = mock_release,
	.read = mock_read,
	.write = mock_write,
	.ioctl = mock_ioctl,
	.poll = mock_poll,
	.mmap = mock_mmap,
};

static const struct lxp_dev mock_dev = {
	.path = "/dev/mock",
	.ops = &mock_ops,
	.major = 42,
	.minor = 7,
	.size = 256, /* a seekable extent (like a framebuffer smem_len) */
};

static uint8_t g_pool[8192] __attribute__((aligned(16)));

/* A rootfs with just a /dev directory so getdents has a directory fd to list. */
static const lxp_file_t g_fs[] = {
	{.path = "/dev", .data = NULL, .size = 0, .mode = LXP_S_IFDIR | 0755u},
};

static void setup(lxp_proc_t *p, ove_arena_t *arena)
{
	assert_int_equal(ove_arena_init(arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(lxp_proc_init(p, arena, 4096), OVE_OK);
	/* All-permitting access_ok range except NULL (region_lo = 1), matching the
	 * syscall-suite harness; a NULL ioctl arg still fails user_ok → -EFAULT. */
	p->region_lo = 1;
	p->region_hi = UINTPTR_MAX;
	p->pool_lo = p->pool_hi = 0;
	lxp_proc_set_rootfs(p, g_fs, 1);
	g_mock_rlen = g_mock_wlen = 0;
	g_mock_block = g_mock_released = g_mock_opened = 0;
	g_mock_val = 0;
	assert_int_equal(lxp_dev_register(&mock_dev), 0); /* idempotent across tests */
}

static long dev_open(lxp_proc_t *p, int flags)
{
	return lxp_syscall(p, LXP_NR_openat, LXP_AT_FDCWD,
			       (long)(uintptr_t) "/dev/mock", flags, 0, 0, 0);
}

/* ---- tests ----------------------------------------------------------------- */
static void test_dev_open_close(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3); /* a fresh fd past the std streams */
	assert_int_equal(p.fds[fd].kind, LXP_FD_DEV);
	assert_int_equal(g_mock_opened, 1);

	/* Opening a non-registered /dev path falls through to ENOENT. */
	assert_int_equal(lxp_syscall(&p, LXP_NR_openat, LXP_AT_FDCWD,
					 (long)(uintptr_t) "/dev/nope", LXP_O_RDWR, 0, 0, 0),
			 -LXP_ENOENT);

	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(g_mock_released, 1);
	assert_int_equal(p.fds[fd].kind, 0 /* FD_FREE */);
}

static void test_dev_read_write(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);

	/* write routes to the driver, which captures the bytes. */
	long w = lxp_syscall(&p, LXP_NR_write, fd, (long)(uintptr_t) "abcd", 4, 0, 0, 0);
	assert_int_equal(w, 4);
	assert_int_equal((int)g_mock_wlen, 4);
	assert_memory_equal(g_mock_wbuf, "abcd", 4);

	/* read returns the driver's canned bytes. */
	memcpy(g_mock_rbuf, "XY", 2);
	g_mock_rlen = 2;
	char rb[8] = {0};
	long r = lxp_syscall(&p, LXP_NR_read, fd, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0);
	assert_int_equal(r, 2);
	assert_memory_equal(rb, "XY", 2);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

static void test_dev_ioctl(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);

	/* SET then GET round-trips the register through the driver. */
	uint32_t v = 0xdeadbeef;
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, MOCK_IOC_SET,
					 (long)(uintptr_t)&v, 0, 0, 0),
			 0);
	uint32_t out = 0;
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, MOCK_IOC_GET,
					 (long)(uintptr_t)&out, 0, 0, 0),
			 0);
	assert_int_equal(out, 0xdeadbeef);

	/* An unknown command → -ENOTTY (not the console gate's blanket reject). */
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, 0x9999, 0, 0, 0, 0),
			 -LXP_ENOTTY);

	/* A bad user pointer (NULL) is rejected by the handler's user_ok → -EFAULT. */
	assert_int_equal(lxp_syscall(&p, LXP_NR_ioctl, fd, MOCK_IOC_GET, 0, 0, 0, 0),
			 -LXP_EFAULT);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

/* mmap(2) of a device buffer (P3): sys_mmap2 routes a /dev fd with an .mmap op to it, which
 * PARKS on DEVW_MMAP — the run-loop coordinator (not present in this unit test) would then
 * install the MPU region + resume with r0 = the mapped address. Assert the deferral state the
 * coordinator consumes (dev_wait/dev_buf/dev_len/dev_cmd), and that a request past the device
 * extent is rejected by the driver op without parking. */
static void test_dev_mmap(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);
	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);

	/* MAP_SHARED (0x1), NOT MAP_ANONYMOUS, PROT_READ|WRITE (0x3) — the fbdev shape. */
	long r = lxp_syscall(&p, LXP_NR_mmap2, 0, 256, 0x3, 0x1, fd, 0);
	assert_int_equal(r, 0); /* parked, not an immediate return */
	assert_int_equal(p.dev_wait, LXP_DEVW_MMAP);
	assert_int_equal(p.dev_buf, (uintptr_t)g_mock_fb);
	assert_int_equal(p.dev_len, 256);
	assert_int_equal(p.dev_cmd, LXP_MAP_NC);

	/* A length past the device extent is rejected by the driver op — no park. */
	p.dev_wait = 0;
	long r2 = lxp_syscall(&p, LXP_NR_mmap2, 0, 512, 0x3, 0x1, fd, 0);
	assert_int_equal(r2, -LXP_EINVAL);
	assert_int_equal(p.dev_wait, 0);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

static void test_dev_stat_lseek_poll(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);

	/* fstat: a character device with the driver's (major,minor) in st_rdev. */
	struct {
		uint64_t st_dev;
		uint8_t pad0[4];
		uint32_t __ino;
		uint32_t st_mode;
		uint32_t st_nlink;
		uint32_t st_uid, st_gid;
		uint64_t st_rdev;
		uint8_t rest[80];
	} st;
	memset(&st, 0, sizeof(st));
	assert_int_equal(
		lxp_syscall(&p, LXP_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0), 0);
	assert_int_equal(st.st_mode & LXP_S_IFMT, LXP_S_IFCHR);
	assert_int_equal((int)st.st_rdev, (42 << 8) | 7);

	/* stat-by-path recognises the device node too. */
	memset(&st, 0, sizeof(st));
	assert_int_equal(lxp_syscall(&p, LXP_NR_stat64, (long)(uintptr_t) "/dev/mock",
					 (long)(uintptr_t)&st, 0, 0, 0, 0),
			 0);
	assert_int_equal(st.st_mode & LXP_S_IFMT, LXP_S_IFCHR);

	/* lseek within the device's fixed extent (size 256). */
	assert_int_equal(lxp_syscall(&p, LXP_NR_lseek, fd, 10, LXP_SEEK_SET, 0, 0, 0),
			 10);
	assert_int_equal(lxp_syscall(&p, LXP_NR_lseek, fd, 0, LXP_SEEK_END, 0, 0, 0),
			 256);
	assert_int_equal(lxp_syscall(&p, LXP_NR_lseek, fd, 300, LXP_SEEK_SET, 0, 0, 0),
			 -LXP_EINVAL); /* past the extent */

	/* poll reports the driver's readiness bits. */
	lxp_pollfd pfd = {.fd = (int)fd, .events = LXP_POLLIN | LXP_POLLOUT};
	long pr = lxp_syscall(&p, LXP_NR_poll, (long)(uintptr_t)&pfd, 1, 0, 0, 0, 0);
	assert_int_equal(pr, 1);
	assert_int_equal(pfd.revents & (LXP_POLLIN | LXP_POLLOUT),
			 LXP_POLLIN | LXP_POLLOUT);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
}

static void test_dev_deferred_block(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);

	/* A blocking read parks the proc (dev_wait set, syscall returns 0 = parked). */
	g_mock_block = 1;
	char rb[8] = {0};
	long r = lxp_syscall(&p, LXP_NR_read, fd, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0);
	assert_int_equal(r, 0);
	assert_int_not_equal(p.dev_wait, 0); /* parked for the coordinator to retry */

	/* Still blocked → the coordinator's retry reports -EAGAIN (stay parked). */
	assert_int_equal(lxp_dev_retry(&p), -LXP_EAGAIN);

	/* Data arrives → the retry completes with the bytes (the run loop then resumes). */
	memcpy(g_mock_rbuf, "ok", 2);
	g_mock_rlen = 2;
	g_mock_block = 0;
	assert_int_equal(lxp_dev_retry(&p), 2);
	assert_memory_equal(rb, "ok", 2);

	/* O_NONBLOCK does NOT park — it returns -EAGAIN straight to the program. */
	p.dev_wait = 0;
	long fd2 = dev_open(&p, LXP_O_RDWR | LXP_O_NONBLOCK);
	assert_true(fd2 >= 3);
	g_mock_block = 1;
	assert_int_equal(
		lxp_syscall(&p, LXP_NR_read, fd2, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0),
		-LXP_EAGAIN);
	assert_int_equal(p.dev_wait, 0);

	lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0);
	lxp_syscall(&p, LXP_NR_close, fd2, 0, 0, 0, 0, 0);
}

static void test_dev_dup_refcount(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	long fd = dev_open(&p, LXP_O_RDWR);
	assert_true(fd >= 3);
	long fd2 = lxp_syscall(&p, LXP_NR_dup, fd, 0, 0, 0, 0, 0);
	assert_true(fd2 >= 3 && fd2 != fd);
	assert_int_equal(p.fds[fd2].kind, LXP_FD_DEV);
	assert_int_equal(p.fds[fd2].file_idx, p.fds[fd].file_idx); /* share the open */

	/* Closing one dup keeps the open alive (refs 2 → 1, no release). */
	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(g_mock_released, 0);
	/* Closing the last reference releases it. */
	assert_int_equal(lxp_syscall(&p, LXP_NR_close, fd2, 0, 0, 0, 0, 0), 0);
	assert_int_equal(g_mock_released, 1);
}

static void test_dev_getdents(void **state)
{
	(void)state;
	ove_arena_t arena;
	lxp_proc_t p;
	setup(&p, &arena);

	/* Open /dev as a directory and list it — the mock device appears as DT_CHR. */
	long dfd = lxp_syscall(&p, LXP_NR_openat, LXP_AT_FDCWD,
				   (long)(uintptr_t) "/dev", LXP_O_RDONLY, 0, 0, 0);
	assert_true(dfd >= 3);

	uint8_t buf[256];
	long n = lxp_syscall(&p, LXP_NR_getdents64, dfd, (long)(uintptr_t)buf, sizeof(buf),
				 0, 0, 0);
	assert_true(n > 0);

	int found = 0;
	for (long off = 0; off < n;) {
		/* linux_dirent64: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[] */
		uint16_t reclen;
		memcpy(&reclen, buf + off + 16, sizeof(reclen));
		uint8_t d_type = buf[off + 18];
		const char *name = (const char *)(buf + off + 19);
		if (strcmp(name, "mock") == 0) {
			found = 1;
			assert_int_equal(d_type, LXP_DT_CHR);
		}
		assert_true(reclen > 0);
		off += reclen;
	}
	assert_true(found);

	lxp_syscall(&p, LXP_NR_close, dfd, 0, 0, 0, 0, 0);
}

int test_linux_dev_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_dev_open_close),
		cmocka_unit_test(test_dev_read_write),
		cmocka_unit_test(test_dev_ioctl),
		cmocka_unit_test(test_dev_mmap),
		cmocka_unit_test(test_dev_stat_lseek_poll),
		cmocka_unit_test(test_dev_deferred_block),
		cmocka_unit_test(test_dev_dup_refcount),
		cmocka_unit_test(test_dev_getdents),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
