/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux personality remote-fs (9P2000.L) tests: drive ove_lnx_syscall() (no
 * hardware SVC, no run loop) against an in-process mock 9P server on a host
 * loopback socket, to check the FD_NET routing — open/read/getdents/stat/lseek/
 * close over /mnt, the walk+getattr+lopen sequence, dirent64 paging, Rlerror →
 * errno, and dup refcounting. The coordinator socket is the POSIX ove_net
 * backend, so this is a hermetic loopback integration test of the whole 9P
 * client state machine (as test_linux_net.c is for sockets).
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "ove/linux/netfs.h"
#include "ove/linux/syscall.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ---- little-endian codec for the mock server ------------------------------- */
static void w16(uint8_t *b, size_t *o, uint16_t v)
{
	b[(*o)++] = (uint8_t)v;
	b[(*o)++] = (uint8_t)(v >> 8);
}
static void w32(uint8_t *b, size_t *o, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b[(*o)++] = (uint8_t)(v >> (8 * i));
}
static void w64(uint8_t *b, size_t *o, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		b[(*o)++] = (uint8_t)(v >> (8 * i));
}
static void wstr(uint8_t *b, size_t *o, const char *s)
{
	size_t n = strlen(s);
	w16(b, o, (uint16_t)n);
	memcpy(b + *o, s, n);
	*o += n;
}
static void wqid(uint8_t *b, size_t *o, int is_dir, uint64_t path)
{
	b[(*o)++] = is_dir ? 0x80 : 0x00; /* qid.type */
	w32(b, o, 0);			  /* qid.version */
	w64(b, o, path);		  /* qid.path */
}
static uint16_t r16(const uint8_t *b, size_t *o)
{
	uint16_t v = (uint16_t)(b[*o] | (b[*o + 1] << 8));
	*o += 2;
	return v;
}
static uint32_t r32(const uint8_t *b, size_t *o)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v |= (uint32_t)b[(*o)++] << (8 * i);
	return v;
}

/* ---- synthetic tree -------------------------------------------------------- */
struct mnode {
	const char *name;
	int is_dir;
	const char *content;
	int parent;
};
#define PROG_CONTENT "\x7f\x45\x4c\x46 fake-fdpic-image-bytes-for-the-exec-fetch-test\n"
static const struct mnode g_tree[] = {
	{"", 1, NULL, -1},		     /* 0: root */
	{"hello.txt", 0, "hello world\n", 0},/* 1 */
	{"sub", 1, NULL, 0},		     /* 2 */
	{"a.txt", 0, "aaa", 2},		     /* 3 */
	{"prog", 0, PROG_CONTENT, 0},	     /* 4: a file the exec-fetch test pulls into staging */
};
#define NTREE ((int)(sizeof(g_tree) / sizeof(g_tree[0])))

static int child_by_name(int parent, const char *name, size_t nlen)
{
	for (int i = 0; i < NTREE; i++)
		if (g_tree[i].parent == parent && strlen(g_tree[i].name) == nlen &&
		    memcmp(g_tree[i].name, name, nlen) == 0)
			return i;
	return -1;
}

/* ---- mock 9P2000.L server (one connection) --------------------------------- */
enum {
	P9_RLERROR = 7,
	P9_TLOPEN = 12,
	P9_TLGETATTR = 24,
	P9_TREADDIR = 40,
	P9_TVERSION = 100,
	P9_TATTACH = 104,
	P9_TWALK = 110,
	P9_TREAD = 116,
	P9_TCLUNK = 120,
};

static int readn(int fd, void *buf, size_t n)
{
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, (uint8_t *)buf + got, n - got);
		if (r <= 0)
			return (int)r;
		got += (size_t)r;
	}
	return (int)got;
}

static void reply(int c, uint8_t *b, size_t len)
{
	size_t o = 0;
	w32(b, &o, (uint32_t)len); /* patch size prefix */
	(void)write(c, b, len);
}

static void rlerror(int c, uint16_t tag, uint32_t ecode)
{
	uint8_t b[16];
	size_t o = 0;
	w32(b, &o, 0);
	b[o++] = P9_RLERROR;
	w16(b, &o, tag);
	w32(b, &o, ecode);
	reply(c, b, o);
}

static void *mock9p(void *arg)
{
	int ls = *(int *)arg;
	int c = accept(ls, NULL, NULL);
	if (c < 0)
		return NULL;
	int fidnode[512];
	for (int i = 0; i < 512; i++)
		fidnode[i] = -1;
	uint8_t in[4096], out[4096];
	for (;;) {
		uint8_t hdr[7];
		if (readn(c, hdr, 7) <= 0)
			break;
		size_t ho = 0;
		uint32_t size = r32(hdr, &ho);
		uint8_t type = hdr[4];
		uint16_t tag = (uint16_t)(hdr[5] | (hdr[6] << 8));
		size_t blen = size - 7;
		if (blen > sizeof(in))
			break;
		if (blen && readn(c, in, blen) <= 0)
			break;
		size_t io = 0, oo = 0;

		if (type == P9_TVERSION) {
			uint32_t ms = r32(in, &io);
			w32(out, &oo, 0);
			out[oo++] = P9_TVERSION + 1;
			w16(out, &oo, tag);
			w32(out, &oo, ms < 2048 ? ms : 2048);
			wstr(out, &oo, "9P2000.L");
			reply(c, out, oo);
		} else if (type == P9_TATTACH) {
			uint32_t fid = r32(in, &io);
			fidnode[fid & 511] = 0; /* root */
			w32(out, &oo, 0);
			out[oo++] = P9_TATTACH + 1;
			w16(out, &oo, tag);
			wqid(out, &oo, 1, 1);
			reply(c, out, oo);
		} else if (type == P9_TWALK) {
			uint32_t fid = r32(in, &io);
			uint32_t newfid = r32(in, &io);
			uint16_t nw = r16(in, &io);
			int node = fidnode[fid & 511];
			int ok = 1, nqid = 0;
			uint8_t qids[16][13];
			for (int i = 0; i < nw; i++) {
				uint16_t l = r16(in, &io);
				int ch = child_by_name(node, (const char *)(in + io), l);
				io += l;
				if (ch < 0) {
					ok = 0;
					break;
				}
				node = ch;
				size_t qo = 0;
				wqid(qids[nqid], &qo, g_tree[ch].is_dir, (uint64_t)(ch + 1));
				nqid++;
			}
			if (!ok && nqid == 0) {
				rlerror(c, tag, 2 /* ENOENT */);
			} else {
				fidnode[newfid & 511] = node;
				w32(out, &oo, 0);
				out[oo++] = P9_TWALK + 1;
				w16(out, &oo, tag);
				w16(out, &oo, (uint16_t)nqid);
				for (int i = 0; i < nqid; i++) {
					memcpy(out + oo, qids[i], 13);
					oo += 13;
				}
				reply(c, out, oo);
			}
		} else if (type == P9_TLGETATTR) {
			uint32_t fid = r32(in, &io);
			int node = fidnode[fid & 511];
			if (node < 0) {
				rlerror(c, tag, 9);
				continue;
			}
			uint32_t mode = g_tree[node].is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
			uint64_t sz = g_tree[node].content ? strlen(g_tree[node].content) : 0;
			w32(out, &oo, 0);
			out[oo++] = P9_TLGETATTR + 1;
			w16(out, &oo, tag);
			w64(out, &oo, 0x7ff);		      /* valid */
			wqid(out, &oo, g_tree[node].is_dir, (uint64_t)(node + 1));
			w32(out, &oo, mode);
			w32(out, &oo, 0);		      /* uid */
			w32(out, &oo, 0);		      /* gid */
			w64(out, &oo, 1);		      /* nlink */
			w64(out, &oo, 0);		      /* rdev */
			w64(out, &oo, sz);		      /* size */
			w64(out, &oo, 512);		      /* blksize */
			w64(out, &oo, (sz + 511) / 512);      /* blocks */
			w64(out, &oo, 0);		      /* atime_sec */
			w64(out, &oo, 0);		      /* atime_nsec */
			w64(out, &oo, 0x5000);		      /* mtime_sec (a recognizable value) */
			w64(out, &oo, 0);		      /* mtime_nsec */
			w64(out, &oo, 0);		      /* ctime_sec */
			w64(out, &oo, 0);		      /* ctime_nsec */
			w64(out, &oo, 0);		      /* btime_sec */
			w64(out, &oo, 0);		      /* btime_nsec */
			w64(out, &oo, 0);		      /* gen */
			w64(out, &oo, 0);		      /* data_version */
			reply(c, out, oo);
		} else if (type == P9_TLOPEN) {
			uint32_t fid = r32(in, &io);
			int node = fidnode[fid & 511];
			w32(out, &oo, 0);
			out[oo++] = P9_TLOPEN + 1;
			w16(out, &oo, tag);
			wqid(out, &oo, node >= 0 && g_tree[node].is_dir, (uint64_t)(node + 1));
			w32(out, &oo, 0); /* iounit */
			reply(c, out, oo);
		} else if (type == P9_TREAD) {
			uint32_t fid = r32(in, &io);
			uint64_t off = 0;
			for (int i = 0; i < 8; i++)
				off |= (uint64_t)in[io++] << (8 * i);
			uint32_t cnt = r32(in, &io);
			int node = fidnode[fid & 511];
			const char *data = (node >= 0) ? g_tree[node].content : NULL;
			size_t total = data ? strlen(data) : 0;
			size_t avail = (off < total) ? total - (size_t)off : 0;
			if (avail > cnt)
				avail = cnt;
			w32(out, &oo, 0);
			out[oo++] = P9_TREAD + 1;
			w16(out, &oo, tag);
			w32(out, &oo, (uint32_t)avail);
			if (avail)
				memcpy(out + oo, data + off, avail);
			oo += avail;
			reply(c, out, oo);
		} else if (type == P9_TREADDIR) {
			uint32_t fid = r32(in, &io);
			uint64_t off = 0;
			for (int i = 0; i < 8; i++)
				off |= (uint64_t)in[io++] << (8 * i);
			int node = fidnode[fid & 511];
			w32(out, &oo, 0);
			out[oo++] = P9_TREADDIR + 1;
			w16(out, &oo, tag);
			size_t cntpos = oo;
			w32(out, &oo, 0); /* count, patched below */
			size_t start = oo;
			uint64_t cookie = 0;
			for (int i = 0; i < NTREE; i++) {
				if (g_tree[i].parent != node)
					continue;
				cookie++;
				if (cookie <= off)
					continue; /* already returned in a prior batch */
				wqid(out, &oo, g_tree[i].is_dir, (uint64_t)(i + 1));
				w64(out, &oo, cookie);				  /* entry offset */
				out[oo++] = g_tree[i].is_dir ? 4 /*DT_DIR*/ : 8; /*DT_REG*/
				wstr(out, &oo, g_tree[i].name);
			}
			uint32_t dlen = (uint32_t)(oo - start);
			size_t tmp = cntpos;
			w32(out, &tmp, dlen);
			reply(c, out, oo);
		} else if (type == P9_TCLUNK) {
			uint32_t fid = r32(in, &io);
			fidnode[fid & 511] = -1;
			w32(out, &oo, 0);
			out[oo++] = P9_TCLUNK + 1;
			w16(out, &oo, tag);
			reply(c, out, oo);
		} else {
			rlerror(c, tag, 38 /* ENOSYS */);
		}
	}
	close(c);
	return NULL;
}

/* ---- harness --------------------------------------------------------------- */
static uint8_t g_pool[8192] __attribute__((aligned(16)));

static void setup(ove_lnx_proc_t *p, ove_arena_t *arena)
{
	assert_int_equal(ove_arena_init(arena, g_pool, sizeof(g_pool)), OVE_OK);
	assert_int_equal(ove_lnx_proc_init(p, arena, 4096), OVE_OK);
	p->region_lo = 1;
	p->region_hi = UINTPTR_MAX;
	p->pool_lo = p->pool_hi = 0;
	p->netfs_req = -1;
}

/* Drive a syscall; if it parked on netfs, pump the coordinator retry. */
static long call_pump(ove_lnx_proc_t *p, long nr, long a0, long a1, long a2, long a3, long a4,
		      long a5)
{
	long r = ove_lnx_syscall(p, nr, a0, a1, a2, a3, a4, a5);
	if (!p->netfs_wait)
		return r;
	for (int i = 0; i < 4000; i++) {
		long rr = ove_lnx_netfs_retry(p);
		if (rr != -OVE_LNX_EAGAIN) {
			p->netfs_wait = 0;
			return rr;
		}
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 300000};
		nanosleep(&ts, NULL);
	}
	p->netfs_wait = 0;
	return -OVE_LNX_EAGAIN;
}

static pthread_t g_mock_thread;
static int g_mock_ls = -1;

static void start_mock_and_mount(void)
{
	g_mock_ls = socket(AF_INET, SOCK_STREAM, 0);
	assert_true(g_mock_ls >= 0);
	int one = 1;
	setsockopt(g_mock_ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert_int_equal(bind(g_mock_ls, (struct sockaddr *)&sa, sizeof(sa)), 0);
	assert_int_equal(listen(g_mock_ls, 1), 0);
	socklen_t sl = sizeof(sa);
	assert_int_equal(getsockname(g_mock_ls, (struct sockaddr *)&sa, &sl), 0);
	int port = ntohs(sa.sin_port);
	assert_int_equal(pthread_create(&g_mock_thread, NULL, mock9p, &g_mock_ls), 0);

	uint8_t ip[4] = {127, 0, 0, 1};
	ove_lnx_netfs_mount_config("/mnt/pi", ip, (uint16_t)port, "/srv", "root");
	ove_lnx_netfs_init(); /* connects + Tversion/Tattach handshake (blocking) */
}

static void stop_mock(void)
{
	/* The coordinator socket has no disconnect API, so the mock thread stays blocked
	 * reading it; detach it and let process teardown reap it (no heap → no ASan leak). */
	pthread_detach(g_mock_thread);
	if (g_mock_ls >= 0)
		close(g_mock_ls);
	g_mock_ls = -1;
}

#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/* The engine staging buffer for a fetched remote ELF (the STM32 backend puts this in SDRAM). */
static uint8_t g_stage[64 * 1024];
uint8_t *ove_lnx_netfs_exec_stage(size_t *cap)
{
	if (cap)
		*cap = sizeof(g_stage);
	return g_stage;
}
#endif

/* ---- test: full browse over one mock connection ---------------------------- */
static void test_netfs_browse(void **state)
{
	(void)state;
	ove_arena_t arena;
	ove_lnx_proc_t p;
	setup(&p, &arena);
	start_mock_and_mount();

	/* stat a file → S_IFREG, size 12, netfs mtime, non-zero inode. */
	struct ove_lnx_kstat64_probe {
		uint64_t st_dev;
		uint8_t pad0[4];
		uint32_t __ino;
		uint32_t st_mode;
		uint32_t st_nlink;
		uint32_t st_uid, st_gid;
		uint64_t st_rdev;
		uint8_t pad3[4];
		int64_t st_size;
		uint32_t st_blksize;
		uint64_t st_blocks;
		uint32_t st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
		uint64_t st_ino;
	} st;
	memset(&st, 0, sizeof(st));
	assert_int_equal(call_pump(&p, OVE_LNX_NR_stat64, (long)(uintptr_t) "/mnt/pi/hello.txt",
				   (long)(uintptr_t)&st, 0, 0, 0, 0),
			 0);
	assert_int_equal(st.st_mode & OVE_LNX_S_IFMT, OVE_LNX_S_IFREG);
	assert_int_equal(st.st_size, 12);
	assert_int_equal(st.st_mtime, 0x5000);
	assert_true(st.st_ino != 0);

	/* stat a missing file → ENOENT (Rlerror). */
	assert_int_equal(call_pump(&p, OVE_LNX_NR_stat64, (long)(uintptr_t) "/mnt/pi/nope",
				   (long)(uintptr_t)&st, 0, 0, 0, 0),
			 -OVE_LNX_ENOENT);

	/* open + read the file → "hello world\n". */
	long fd = call_pump(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
			    (long)(uintptr_t) "/mnt/pi/hello.txt", OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(fd >= 3);
	assert_int_equal(p.fds[fd].kind, OVE_LNX_FD_NET);
	char rb[32] = {0};
	long got = call_pump(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0);
	assert_int_equal(got, 12);
	assert_memory_equal(rb, "hello world\n", 12);
	/* a second read at EOF returns 0. */
	assert_int_equal(call_pump(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0),
			 0);

	/* fstat the open fd → cached attrs. */
	memset(&st, 0, sizeof(st));
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_fstat64, fd, (long)(uintptr_t)&st, 0, 0, 0, 0),
			 0);
	assert_int_equal(st.st_size, 12);

	/* lseek(SEEK_SET, 6) then read → "world\n". */
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_lseek, fd, 6, OVE_LNX_SEEK_SET, 0, 0, 0), 6);
	memset(rb, 0, sizeof(rb));
	got = call_pump(&p, OVE_LNX_NR_read, fd, (long)(uintptr_t)rb, sizeof(rb), 0, 0, 0);
	assert_int_equal(got, 6);
	assert_memory_equal(rb, "world\n", 6);

	/* dup shares the open (same file_idx); closing one keeps it. */
	long fd2 = ove_lnx_syscall(&p, OVE_LNX_NR_dup, fd, 0, 0, 0, 0, 0);
	assert_true(fd2 >= 3 && fd2 != fd);
	assert_int_equal(p.fds[fd2].file_idx, p.fds[fd].file_idx);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd, 0, 0, 0, 0, 0), 0);
	assert_int_equal(ove_lnx_syscall(&p, OVE_LNX_NR_close, fd2, 0, 0, 0, 0, 0), 0);

	/* getdents64 on the mount root → sees hello.txt + sub. */
	long dfd = call_pump(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD, (long)(uintptr_t) "/mnt/pi",
			     OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(dfd >= 3);
	uint8_t dbuf[512] = {0};
	long dn = call_pump(&p, OVE_LNX_NR_getdents64, dfd, (long)(uintptr_t)dbuf, sizeof(dbuf), 0, 0,
			    0);
	assert_true(dn > 0);
	/* scan the dirent64 records for the two names. */
	int saw_hello = 0, saw_sub = 0;
	for (size_t off = 0; off + 19 < (size_t)dn;) {
		uint16_t reclen = (uint16_t)(dbuf[off + 16] | (dbuf[off + 17] << 8));
		if (reclen == 0)
			break;
		const char *name = (const char *)(dbuf + off + 19);
		if (strcmp(name, "hello.txt") == 0)
			saw_hello = 1;
		if (strcmp(name, "sub") == 0)
			saw_sub = 1;
		off += reclen;
	}
	assert_true(saw_hello);
	assert_true(saw_sub);
	/* a second getdents at EOF returns 0. */
	assert_int_equal(
		call_pump(&p, OVE_LNX_NR_getdents64, dfd, (long)(uintptr_t)dbuf, sizeof(dbuf), 0, 0, 0),
		0);
	ove_lnx_syscall(&p, OVE_LNX_NR_close, dfd, 0, 0, 0, 0, 0);

	/* a write to a netfs fd is rejected read-only. */
	long wfd = call_pump(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
			     (long)(uintptr_t) "/mnt/pi/hello.txt", OVE_LNX_O_RDONLY, 0, 0, 0);
	assert_true(wfd >= 3);
	assert_int_equal(
		ove_lnx_syscall(&p, OVE_LNX_NR_write, wfd, (long)(uintptr_t) "x", 1, 0, 0, 0),
		-OVE_LNX_EROFS);
	ove_lnx_syscall(&p, OVE_LNX_NR_close, wfd, 0, 0, 0, 0, 0);

	/* an O_WRONLY open of a netfs path is rejected read-only. */
	assert_int_equal(call_pump(&p, OVE_LNX_NR_openat, OVE_LNX_AT_FDCWD,
				   (long)(uintptr_t) "/mnt/pi/hello.txt", OVE_LNX_O_WRONLY, 0, 0, 0),
			 -OVE_LNX_EROFS);

#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
	/* exec-fetch: execve("/mnt/pi/prog") pulls the whole file into the staging buffer and, on
	 * completion, the retry sets exec_pending + a SENTINEL exec_file_idx (it does NOT resume —
	 * the run loop's EV_EXEC launches from the staging buffer). */
	{
		ove_lnx_proc_t xp;
		ove_arena_t xa;
		setup(&xp, &xa);
		long xr = ove_lnx_netfs_exec_fetch(&xp, "/mnt/pi/prog");
		assert_int_equal(xr, 0); /* parked */
		assert_int_equal(xp.netfs_wait, OVE_LNX_NETFSW_EXECFETCH);
		long xrr = -OVE_LNX_EAGAIN;
		for (int i = 0; i < 4000 && xrr == -OVE_LNX_EAGAIN; i++) {
			xrr = ove_lnx_netfs_retry(&xp);
			if (xrr == -OVE_LNX_EAGAIN) {
				struct timespec ts = {0, 300000};
				nanosleep(&ts, NULL);
			}
		}
		assert_int_equal(xrr, 0);
		assert_int_equal(xp.exec_pending, 1);
		assert_int_equal(xp.exec_file_idx, OVE_LNX_NETFS_EXEC_SENTINEL);
		size_t xsz = 0;
		const uint8_t *ximg = ove_lnx_netfs_exec_image(&xsz);
		assert_int_equal((int)xsz, (int)(sizeof(PROG_CONTENT) - 1));
		assert_memory_equal(ximg, PROG_CONTENT, sizeof(PROG_CONTENT) - 1);
	}
#endif

	/* drain any background clunks, then let the mock connection close. */
	for (int i = 0; i < 50; i++) {
		uint64_t now = (uint64_t)i * 1000;
		ove_lnx_netfs_tick(now);
	}
	/* proc_exit releases the coordinator socket's peer by closing our end via the
	 * kernel when the test process tears down; signal the mock to stop by closing. */
	stop_mock();
}

int test_linux_netfs_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_netfs_browse),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
