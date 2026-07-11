/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux-personality remote filesystem: a coordinator-owned 9P2000.L client over a
 * single non-blocking TCP connection to a 9P server (diod), exposed as an FD_NET
 * provider the syscall handlers route /mnt/pi opens to. Read-only browse (+ exec
 * off the mount, Phase B). Mirrors linux/net/ove_linux_net.c: a refcounted per-open
 * pool (each = a 9P fid + cursor), fork/dup share, the last close clunks.
 *
 * Blocking is deferred, never inline: every op that needs a Pi round-trip submits a
 * 9P request, sets proc->netfs_wait, and returns 0 (parked); the run-loop coordinator
 * pumps the transport each pass via lxp_netfs_retry and resumes the guest. The
 * transport is serialized — one 9P message in flight, a FIFO of the rest.
 */

#include "lxp/lxp_config.h"

#if defined(LXP_ENABLE_NETFS)

#include "lxp/lxp_netfs.h"
#include "lxp/lxp_port.h"
#include "lxp/lxp_net_ops.h"
#include "lxp/lxp_types.h"

#include <string.h>

/* ---- tunables -------------------------------------------------------------- */
/* These sit in scarce internal SRAM, so keep the footprint small: msize 2K (two 2K
 * transport buffers) is plenty for read-only browse (reads/readdir just page in more
 * chunks). A future optimization relocates the buffers to SDRAM for larger msize. */
#define NETFS_MSIZE 2048u	 /* negotiated max 9P message; two buffers of this. */
#define NETFS_NFID 32		 /* max concurrent 9P fids (root + opens + temp). */
#define NETFS_NOPEN 16		 /* max concurrent guest opens (pooled). */
#define NETFS_NREQ 8		 /* max in-flight+queued requests (parked procs). */
#define NETFS_NCLUNK 24		 /* pending background-clunk fid queue. */
#define NETFS_MP_MAX 64		 /* mount-point string cap. */
#define NETFS_NAME_MAX 96	 /* one path-component / attach-string cap. */
#define NETFS_RECONNECT_US 2000000ull /* backoff between reconnect attempts. */
#define NETFS_MAXWELEM 16	 /* 9P Twalk max name components per message. */

#define P9_NOTAG 0xffffu
#define P9_NOFID 0xffffffffu
#define P9_TAG 1u /* one request in flight → a single fixed tag. */
#define P9_ROOT_FID 0u
#define P9_GETATTR_BASIC 0x000007ffull /* mode,nlink,uid,gid,rdev,atime,mtime,ctime,ino,size,blocks */

/* 9P2000.L message types. */
enum {
	P9_RLERROR = 7,
	P9_TLOPEN = 12,
	P9_RLOPEN = 13,
	P9_TLGETATTR = 24,
	P9_RLGETATTR = 25,
	P9_TREADDIR = 40,
	P9_RREADDIR = 41,
	P9_TVERSION = 100,
	P9_RVERSION = 101,
	P9_TATTACH = 104,
	P9_RATTACH = 105,
	P9_TWALK = 110,
	P9_RWALK = 111,
	P9_TREAD = 116,
	P9_RREAD = 117,
	P9_TCLUNK = 120,
	P9_RCLUNK = 121,
};

/* qid.type bits. */
#define P9_QTDIR 0x80u

/* ---- mount config + connection state --------------------------------------- */

static struct {
	char mp[NETFS_MP_MAX]; /* mount point, e.g. "/mnt/pi" */
	size_t mplen;
	uint8_t ip[4];
	uint16_t port;
	char aname[NETFS_NAME_MAX];
	char uname[32];
	int configured;
} g_mnt;

enum { CONN_DOWN = 0, CONN_UP };
static lxp_socket_t g_sk; /* host-owned handle; the adapter owns the storage */
static int g_conn;
static uint32_t g_msize = NETFS_MSIZE;
static uint32_t g_generation;	    /* bumped on every (re)connect; opens carry theirs. */
static uint64_t g_reconnect_at_us;  /* next reconnect attempt (backoff). */

/* ---- fid allocator (bitmap; fid 0 = attached root) ------------------------- */
static uint32_t g_fid_bm[(NETFS_NFID + 31) / 32];

static int fid_alloc(void)
{
	for (int f = 1; f < NETFS_NFID; f++)
		if (!(g_fid_bm[f >> 5] & (1u << (f & 31)))) {
			g_fid_bm[f >> 5] |= (1u << (f & 31));
			return f;
		}
	return -1;
}

static void fid_free(int f)
{
	if (f > 0 && f < NETFS_NFID)
		g_fid_bm[f >> 5] &= ~(1u << (f & 31));
}

/* ---- open pool ------------------------------------------------------------- */
struct netfs_open {
	uint8_t used;
	uint8_t refs;	/* fork/dup share; last drop enqueues a clunk. */
	uint8_t is_dir;
	uint8_t stale;	/* fid invalidated by a reconnect → read/getdents give -ESTALE. */
	int fid;
	uint32_t generation;
	uint32_t mode;	/* cached st_mode (from Rlgetattr at open). */
	uint64_t size;
	uint64_t mtime; /* seconds. */
	uint64_t ino;	/* qid.path — stable + unique on the server. */
	uint64_t rd_off;  /* file read cursor (shared across dup/fork — POSIX open-file offset). */
	uint64_t dir_off; /* Treaddir resume cursor. */
};
static struct netfs_open g_open[NETFS_NOPEN];

static struct netfs_open *open_slot(int oi)
{
	if (oi < 0 || oi >= NETFS_NOPEN || !g_open[oi].used)
		return NULL;
	return &g_open[oi];
}

/* ---- background clunk queue (close never parks) ---------------------------- */
static int g_clunk_fid[NETFS_NCLUNK];
static int g_clunk_head, g_clunk_tail;

static void clunk_enqueue(int fid)
{
	int nt = (g_clunk_tail + 1) % NETFS_NCLUNK;
	if (fid <= 0)
		return;
	if (nt == g_clunk_head) {
		fid_free(fid); /* queue full: drop the fid locally (server GCs on disconnect). */
		return;
	}
	g_clunk_fid[g_clunk_tail] = fid;
	g_clunk_tail = nt;
}

/* ---- request pool ---------------------------------------------------------- */
enum { REQ_FREE = 0, REQ_QUEUED, REQ_INFLIGHT, REQ_DONE };
#define REQ_OP_CLUNK 0xffu /* internal (owner_slot<0) background-clunk request. */
struct netfs_req {
	uint8_t state;
	uint8_t op;    /* NETFSW_* (owner set) or REQ_OP_CLUNK (owner NULL). */
	uint8_t step;
	lxp_proc_t *owner; /* proc to resume/marshal for, or NULL for an internal (clunk) request. */
	uint32_t seq;	/* FIFO ordering. */
	int oi;		/* open-pool slot (open reserves it; read/getdents use it). */
	int fid;	/* working fid (walk target / temp). */
	uint64_t off;	/* read / readdir offset. */
	uintptr_t ubuf; /* guest buffer (read/getdents) or stat-out. */
	size_t ulen;	/* length / capacity. */
	int flags;	/* open flags / statkind / is64. */
	int statkind;
	int is64;
	char path[LXP_PATH_MAX]; /* remote path (open/stat). */
	long result;
};
static struct netfs_req g_req[NETFS_NREQ];
static uint32_t g_req_seq;
static int g_inflight = -1; /* request whose message is being sent / awaiting reply. */

#if defined(LXP_ENABLE_NETFS_EXEC)
static uint8_t *g_exec_buf;  /* the engine's RAM staging buffer for a fetched remote ELF */
static size_t g_exec_cap;    /* staging capacity */
static size_t g_exec_size;   /* bytes fetched so far (the valid image size on completion) */
#endif

/* ---- TX/RX transport buffers ----------------------------------------------- */
static uint8_t g_tx[NETFS_MSIZE];
static size_t g_txlen, g_txoff; /* current outgoing message [g_txoff, g_txlen). */
static uint8_t g_rx[NETFS_MSIZE];
static size_t g_rxlen; /* accumulated bytes of the incoming reply. */

/* ---- little-endian 9P codec ------------------------------------------------ */
static void put8(size_t *o, uint8_t v)
{
	g_tx[(*o)++] = v;
}
static void put16(size_t *o, uint16_t v)
{
	put8(o, (uint8_t)v);
	put8(o, (uint8_t)(v >> 8));
}
static void put32(size_t *o, uint32_t v)
{
	put16(o, (uint16_t)v);
	put16(o, (uint16_t)(v >> 16));
}
static void put64(size_t *o, uint64_t v)
{
	put32(o, (uint32_t)v);
	put32(o, (uint32_t)(v >> 32));
}
static void putstr(size_t *o, const char *s, size_t n)
{
	put16(o, (uint16_t)n);
	memcpy(g_tx + *o, s, n);
	*o += n;
}

static uint8_t get8(const uint8_t *b, size_t *o)
{
	return b[(*o)++];
}
static uint16_t get16(const uint8_t *b, size_t *o)
{
	uint16_t v = (uint16_t)(b[*o] | (b[*o + 1] << 8));
	*o += 2;
	return v;
}
static uint32_t get32(const uint8_t *b, size_t *o)
{
	uint32_t v = get16(b, o);
	v |= (uint32_t)get16(b, o) << 16;
	return v;
}
static uint64_t get64(const uint8_t *b, size_t *o)
{
	uint64_t v = get32(b, o);
	v |= (uint64_t)get32(b, o) << 32;
	return v;
}

/* Start building a 9P message: reserve size[4]+type[1]+tag[2]; returns the body offset. */
static size_t msg_begin(uint8_t type, uint16_t tag)
{
	size_t o = 0;
	put32(&o, 0); /* size, patched by msg_end */
	put8(&o, type);
	put16(&o, tag);
	return o;
}
static void msg_end(size_t o)
{
	size_t p = 0;
	put32(&p, (uint32_t)o); /* patch the size prefix */
	g_txlen = o;
	g_txoff = 0;
}

/* ---- path splitting -------------------------------------------------------- */
/* Split a remote path (mount-relative, absolute, e.g. "/a/b") into components.
 * Returns the count, or -1 if >NETFS_MAXWELEM. */
static int path_split(const char *path, const char **comp, size_t *len, int max)
{
	int n = 0;
	const char *s = path;
	while (*s == '/')
		s++;
	while (*s) {
		if (n >= max)
			return -1;
		const char *start = s;
		while (*s && *s != '/')
			s++;
		comp[n] = start;
		len[n] = (size_t)(s - start);
		n++;
		while (*s == '/')
			s++;
	}
	return n;
}

/* ---- low-level socket flush / drain (non-blocking) ------------------------- */
/* Returns 1 if the whole pending TX message is flushed, 0 if more to send later,
 * -1 on a transport error (caller drops the connection). */
static int tx_flush(void)
{
	while (g_txoff < g_txlen) {
		size_t sent = 0;
		int r = g_lxp_net_ops->sock_send(g_sk, g_tx + g_txoff, g_txlen - g_txoff, &sent);
		if (r == LXP_OK) {
			g_txoff += sent;
			if (sent == 0)
				return 0; /* nothing accepted now */
			continue;
		}
		if (r == LXP_ERR_TIMEOUT)
			return 0; /* send buffer full → retry next pump */
		return -1;
	}
	return 1;
}

/* Read available bytes into g_rx. Returns 1 if progress, 0 if would-block, -1 on
 * error/peer-close. */
static int rx_fill(void)
{
	int progress = 0;
	while (g_rxlen < sizeof(g_rx)) {
		size_t got = 0;
		int r = g_lxp_net_ops->sock_recv(g_sk, g_rx + g_rxlen, sizeof(g_rx) - g_rxlen, &got,
					LXP_WAIT_FOREVER);
		if (r == LXP_OK) {
			if (got == 0)
				return -1; /* orderly close */
			g_rxlen += got;
			progress = 1;
			continue;
		}
		if (r == LXP_ERR_TIMEOUT)
			return progress;
		return -1;
	}
	return progress;
}

/* Extract one complete 9P reply from g_rx if present. Returns the message length
 * (with the 7-byte header), or 0 if incomplete. On return the message occupies
 * g_rx[0..len); the caller consumes it via rx_consume(). */
static size_t rx_message(void)
{
	if (g_rxlen < 7)
		return 0;
	size_t o = 0;
	uint32_t size = get32(g_rx, &o);
	if (size < 7 || size > sizeof(g_rx))
		return (size_t)-1; /* framing error → reconnect */
	if (g_rxlen < size)
		return 0;
	return size;
}
static void rx_consume(size_t len)
{
	if (len && len <= g_rxlen) {
		memmove(g_rx, g_rx + len, g_rxlen - len);
		g_rxlen -= len;
	}
}

/* ---- connection teardown / (re)connect ------------------------------------- */
static void conn_drop(void)
{
	if (g_sk)
		g_lxp_net_ops->sock_close(g_sk);
	g_sk = NULL;
	g_conn = CONN_DOWN;
	g_txlen = g_txoff = g_rxlen = 0;
	/* Fail every outstanding request; opens become stale. */
	for (int i = 0; i < NETFS_NREQ; i++)
		if (g_req[i].state == REQ_QUEUED || g_req[i].state == REQ_INFLIGHT) {
			g_req[i].result = -LXP_EIO;
			g_req[i].state = REQ_DONE;
		}
	g_inflight = -1;
	for (int i = 0; i < NETFS_NOPEN; i++)
		if (g_open[i].used)
			g_open[i].stale = 1;
}

/* Blocking exchange for the boot handshake: flush g_tx then wait (bounded) for one
 * full reply into g_rx. Returns the reply length or -1. */
static long xchg_blocking(uint64_t timeout_us)
{
	uint64_t now = 0, deadline;
	lxp_time_us(&now);
	deadline = now + timeout_us;
	while (g_txoff < g_txlen) {
		if (tx_flush() < 0)
			return -1;
		lxp_time_us(&now);
		if (now >= deadline)
			return -1;
	}
	for (;;) {
		unsigned rev = 0;
		g_lxp_net_ops->sock_poll(g_sk, LXP_SOCK_POLLIN, &rev, 50 * 1000000ull /* 50ms */);
		if (rx_fill() < 0)
			return -1;
		size_t len = rx_message();
		if (len == (size_t)-1)
			return -1;
		if (len)
			return (long)len;
		lxp_time_us(&now);
		if (now >= deadline)
			return -1;
	}
}

static int handshake(void)
{
	/* Tversion(msize, "9P2000.L") */
	static const char VER[] = "9P2000.L";
	size_t o = msg_begin(P9_TVERSION, P9_NOTAG);
	put32(&o, NETFS_MSIZE);
	putstr(&o, VER, sizeof(VER) - 1);
	msg_end(o);
	if (xchg_blocking(3000000ull) < 0)
		return -1;
	{
		size_t p = 7;
		if (g_rx[4] != P9_RVERSION)
			return -1;
		uint32_t sm = get32(g_rx, &p);
		g_msize = sm < NETFS_MSIZE ? sm : NETFS_MSIZE;
		if (g_msize < 512)
			return -1;
	}
	rx_consume(rx_message());

	/* Tattach(root_fid, NOFID, uname, aname, n_uname=0) */
	o = msg_begin(P9_TATTACH, P9_TAG);
	put32(&o, P9_ROOT_FID);
	put32(&o, P9_NOFID);
	putstr(&o, g_mnt.uname, strlen(g_mnt.uname));
	putstr(&o, g_mnt.aname, strlen(g_mnt.aname));
	put32(&o, 0); /* n_uname (numeric uid) */
	msg_end(o);
	if (xchg_blocking(3000000ull) < 0)
		return -1;
	if (g_rx[4] != P9_RATTACH) {
		rx_consume(rx_message());
		return -1;
	}
	rx_consume(rx_message());
	return 0;
}

/* Build an IPv4 lxp_sockaddr_t (host-order port) — was ove_sockaddr_ipv4, now module-local
 * so the netfs client depends only on the net-ops port, not the ove_net helper set. */
static void netfs_sockaddr_ipv4(lxp_sockaddr_t *a, uint8_t b0, uint8_t b1, uint8_t b2,
				uint8_t b3, uint16_t port)
{
	memset(a, 0, sizeof(*a));
	a->family = LXP_AF_INET;
	a->port = port;
	a->addr[0] = b0;
	a->addr[1] = b1;
	a->addr[2] = b2;
	a->addr[3] = b3;
}

static void conn_connect(uint64_t now_us)
{
	if (!g_mnt.configured)
		return;
	if (now_us < g_reconnect_at_us)
		return;
	g_reconnect_at_us = now_us + NETFS_RECONNECT_US;

	if (g_lxp_net_ops->sock_open(LXP_AF_INET, LXP_SOCK_STREAM, 0, &g_sk) != LXP_OK) {
		g_sk = NULL;
		return;
	}
	lxp_sockaddr_t peer;
	netfs_sockaddr_ipv4(&peer, g_mnt.ip[0], g_mnt.ip[1], g_mnt.ip[2], g_mnt.ip[3], g_mnt.port);
	if (g_lxp_net_ops->sock_connect(g_sk, &peer, 5000000000ull /* 5s */) != LXP_OK) {
		g_lxp_net_ops->sock_close(g_sk);
		g_sk = NULL;
		return;
	}
	g_lxp_net_ops->sock_set_nonblock(g_sk, 1);
	memset(g_fid_bm, 0, sizeof(g_fid_bm));
	g_txlen = g_txoff = g_rxlen = 0;
	if (handshake() != 0) {
		g_lxp_net_ops->sock_close(g_sk);
		g_sk = NULL;
		return;
	}
	g_conn = CONN_UP;
	g_generation++;
}

/* ---- request build for the current step ------------------------------------ */
/* Build the message for req's current (op, step) into g_tx. Returns 0 on success,
 * or a negative errno that completes the request. */
static long req_build(struct netfs_req *r)
{
	const char *comp[NETFS_MAXWELEM];
	size_t clen[NETFS_MAXWELEM];
	size_t o;

	switch (r->op) {
	case LXP_NETFSW_OPEN:
	case LXP_NETFSW_STAT:
#if defined(LXP_ENABLE_NETFS_EXEC)
	case LXP_NETFSW_EXECFETCH:
#endif
		if (r->step == 0) { /* Twalk(root -> fid, components) */
			int n = path_split(r->path, comp, clen, NETFS_MAXWELEM);
			if (n < 0)
				return -LXP_ENAMETOOLONG;
			r->fid = fid_alloc();
			if (r->fid < 0)
				return -LXP_EMFILE;
			o = msg_begin(P9_TWALK, P9_TAG);
			put32(&o, P9_ROOT_FID);
			put32(&o, (uint32_t)r->fid);
			put16(&o, (uint16_t)n);
			for (int i = 0; i < n; i++)
				putstr(&o, comp[i], clen[i]);
			msg_end(o);
			return 0;
		}
		if (r->step == 1) { /* Tlgetattr(fid) */
			o = msg_begin(P9_TLGETATTR, P9_TAG);
			put32(&o, (uint32_t)r->fid);
			put64(&o, P9_GETATTR_BASIC);
			msg_end(o);
			return 0;
		}
		if (r->step == 2) {
			if (r->op == LXP_NETFSW_STAT) { /* stat: clunk the temp fid */
				o = msg_begin(P9_TCLUNK, P9_TAG);
				put32(&o, (uint32_t)r->fid);
				msg_end(o);
				return 0;
			}
			/* OPEN + EXECFETCH: Tlopen(fid, O_RDONLY). Always O_RDONLY — files read via Tread,
			 * directories via Treaddir on the same open fid. NB: do NOT send O_DIRECTORY: its
			 * ARM value is 0x4000 (0x10000 is O_DIRECT on ARM), and a diod dir opened O_DIRECT
			 * fails Treaddir with -EIO. A plain O_RDONLY dir open reads fine. */
			o = msg_begin(P9_TLOPEN, P9_TAG);
			put32(&o, (uint32_t)r->fid);
			put32(&o, 0u);
			msg_end(o);
			return 0;
		}
#if defined(LXP_ENABLE_NETFS_EXEC)
		if (r->step == 3) { /* EXECFETCH: Tread the next chunk into the staging buffer */
			uint32_t cnt = (uint32_t)(g_msize - 24);
			if (r->off + cnt > g_exec_cap) /* never overflow the staging buffer */
				cnt = (uint32_t)(g_exec_cap - r->off);
			o = msg_begin(P9_TREAD, P9_TAG);
			put32(&o, (uint32_t)r->fid);
			put64(&o, r->off);
			put32(&o, cnt);
			msg_end(o);
			return 0;
		}
		if (r->step == 4) { /* EXECFETCH: clunk after EOF */
			o = msg_begin(P9_TCLUNK, P9_TAG);
			put32(&o, (uint32_t)r->fid);
			msg_end(o);
			return 0;
		}
#endif
		return -LXP_EINVAL;

	case LXP_NETFSW_READ: {
		struct netfs_open *op = open_slot(r->oi);
		if (!op)
			return -LXP_EBADF;
		uint32_t cnt = (uint32_t)r->ulen;
		if (cnt > g_msize - 24)
			cnt = g_msize - 24;
		o = msg_begin(P9_TREAD, P9_TAG);
		put32(&o, (uint32_t)op->fid);
		put64(&o, op->rd_off);
		put32(&o, cnt);
		msg_end(o);
		return 0;
	}
	case LXP_NETFSW_GETDENTS: {
		struct netfs_open *op = open_slot(r->oi);
		if (!op)
			return -LXP_EBADF;
		/* Treaddir count must leave room for the 9P read header (P9_IOHDRSZ = 24) within msize,
		 * else diod Rlerrors — same cap as Tread above (a bigger count here = an empty ls). */
		uint32_t cnt = (uint32_t)(g_msize - 24);
		o = msg_begin(P9_TREADDIR, P9_TAG);
		put32(&o, (uint32_t)op->fid);
		put64(&o, op->dir_off);
		put32(&o, cnt);
		msg_end(o);
		return 0;
	}
	default: /* REQ_OP_CLUNK */
		o = msg_begin(P9_TCLUNK, P9_TAG);
		put32(&o, (uint32_t)r->fid);
		msg_end(o);
		return 0;
	}
}

/* ---- dirent emit ----------------------------------------------------------- */
/* Write one directory record to ubuf+off in either the getdents64 layout (is64:
 * d_ino[8] d_off[8] d_reclen[2] d_type[1] name[]\0) or the legacy 32-bit getdents
 * layout (d_ino[4] d_off[4] d_reclen[2] name[]\0 ... d_type at reclen-1). Returns
 * the record length, or 0 if it does not fit. */
static size_t dirent_emit_rec(int is64, uintptr_t ubuf, size_t cap, size_t off, uint64_t ino,
			      uint64_t doff, uint8_t type, const char *name, size_t nlen)
{
	size_t head = is64 ? 19 : 10;
	size_t reclen = (head + nlen + 1 + (is64 ? 0 : 1) + 7) & ~(size_t)7;
	if (off + reclen > cap)
		return 0;
	uint8_t *d = (uint8_t *)(ubuf + off);
	size_t p = 0;
	int inobytes = is64 ? 8 : 4;
	for (int i = 0; i < inobytes; i++)
		d[p++] = (uint8_t)(ino >> (8 * i));
	for (int i = 0; i < inobytes; i++)
		d[p++] = (uint8_t)(doff >> (8 * i));
	d[p++] = (uint8_t)reclen;
	d[p++] = (uint8_t)(reclen >> 8);
	if (is64)
		d[p++] = type;
	memcpy(d + p, name, nlen);
	p += nlen;
	while (p < reclen)
		d[p++] = 0;
	if (!is64)
		d[reclen - 1] = type; /* 32-bit getdents d_type hack: last byte of the record */
	return reclen;
}

/* Map a 9P readdir entry type (a qid.type byte) to a Linux d_type. */
static uint8_t dtype_from_qid(uint8_t qt)
{
	if (qt & P9_QTDIR)
		return LXP_DT_DIR;
	if (qt & 0x02u /* QTSYMLINK */)
		return 10 /* DT_LNK */;
	return LXP_DT_REG;
}

/* ---- reply handling (advances or completes the in-flight request) ---------- */
static void req_complete(struct netfs_req *r, long result)
{
	r->result = result;
	r->state = REQ_DONE;
	g_inflight = -1;
}

/* Parse Rlgetattr body (cursor at first field after the 7-byte header) into attrs. */
static void parse_getattr(const uint8_t *b, size_t o, uint32_t *mode, uint64_t *size,
			  uint64_t *mtime, uint64_t *ino)
{
	(void)get64(b, &o); /* valid */
	uint8_t qtype = get8(b, &o);
	(void)get32(b, &o); /* qid.version */
	uint64_t qpath = get64(b, &o);
	uint32_t m = get32(b, &o);
	(void)get32(b, &o); /* uid */
	(void)get32(b, &o); /* gid */
	(void)get64(b, &o); /* nlink */
	(void)get64(b, &o); /* rdev */
	uint64_t sz = get64(b, &o);
	(void)get64(b, &o); /* blksize */
	(void)get64(b, &o); /* blocks */
	(void)get64(b, &o); /* atime_sec */
	(void)get64(b, &o); /* atime_nsec */
	uint64_t mt = get64(b, &o);
	*mode = m;
	*size = sz;
	*mtime = mt;
	*ino = qpath;
	(void)qtype;
}

static void handle_reply(struct netfs_req *r, uint8_t type, const uint8_t *body, size_t blen)
{
	size_t o = 0;
	(void)blen;

	if (type == P9_RLERROR) {
		uint32_t ecode = get32(body, &o);
		/* clean up any fid this op had walked to */
		if (r->fid > 0) {
			clunk_enqueue(r->fid);
			r->fid = -1;
		}
		if (r->op == LXP_NETFSW_OPEN && r->oi >= 0)
			g_open[r->oi].used = 0;
		req_complete(r, -(long)ecode);
		return;
	}

	switch (r->op) {
	case LXP_NETFSW_OPEN:
		if (r->step == 0) { /* Rwalk: nwqid must equal the requested component count */
			uint16_t nwqid = get16(body, &o);
			const char *cp[NETFS_MAXWELEM];
			size_t cl[NETFS_MAXWELEM];
			int n = path_split(r->path, cp, cl, NETFS_MAXWELEM);
			if (n > 0 && nwqid < n) { /* a component did not resolve */
				clunk_enqueue(r->fid);
				r->fid = -1;
				g_open[r->oi].used = 0;
				req_complete(r, -LXP_ENOENT);
				return;
			}
			r->step = 1;
			return; /* stay inflight; pump rebuilds Tlgetattr */
		}
		if (r->step == 1) { /* Rlgetattr */
			uint32_t mode;
			uint64_t size, mtime, ino;
			parse_getattr(body, o, &mode, &size, &mtime, &ino);
			struct netfs_open *op = &g_open[r->oi];
			op->is_dir = (mode & LXP_S_IFMT) == LXP_S_IFDIR;
			op->mode = mode;
			op->size = size;
			op->mtime = mtime;
			op->ino = ino;
			r->step = 2;
			return;
		}
		if (r->step == 2) { /* Rlopen → open is ready */
			struct netfs_open *op = &g_open[r->oi];
			op->used = 1;
			op->refs = 1;
			op->stale = 0;
			op->fid = r->fid;
			op->generation = g_generation;
			op->dir_off = 0;
			req_complete(r, r->oi); /* retry installs the fd */
			return;
		}
		break;

	case LXP_NETFSW_READ: {
		uint32_t cnt = get32(body, &o);
		if (cnt > blen - 4)
			cnt = (uint32_t)(blen - 4);
		if (cnt > r->ulen)
			cnt = (uint32_t)r->ulen;
		if (cnt)
			memcpy((void *)r->ubuf, body + o, cnt);
		struct netfs_open *op = open_slot(r->oi);
		if (op)
			op->rd_off += cnt; /* advance the shared file cursor */
		req_complete(r, (long)cnt);
		return;
	}
	case LXP_NETFSW_GETDENTS: {
		uint32_t cnt = get32(body, &o); /* bytes of readdir data */
		size_t end = o + cnt;
		size_t filled = 0;
		struct netfs_open *op = open_slot(r->oi);
		uint64_t last_off = op ? op->dir_off : 0;
		while (o < end && o + 13 + 8 + 1 + 2 <= blen) {
			/* entry := qid[13] offset[8] type[1] name[s]; qid := type[1] ver[4] path[8] */
			uint8_t qt = body[o];
			uint64_t qpath = 0;
			for (int i = 0; i < 8; i++)
				qpath |= (uint64_t)body[o + 5 + i] << (8 * i);
			o += 13;
			uint64_t doff = get64(body, &o);
			uint8_t dt9 = get8(body, &o); /* the Linux d_type (DT_*), 0 = unknown */
			uint16_t nlen = get16(body, &o);
			if (o + nlen > blen)
				break;
			const char *nm = (const char *)(body + o);
			o += nlen;
			uint8_t dtype = dt9 ? dt9 : dtype_from_qid(qt);
			size_t rl = dirent_emit_rec(r->is64, r->ubuf, r->ulen, filled, qpath, doff, dtype,
						    nm, nlen);
			if (!rl) /* record didn't fit; stop before it, resume here next call */
				break;
			filled += rl;
			last_off = doff;
		}
		if (op)
			op->dir_off = last_off; /* resume cursor (EOF when cnt==0 → filled 0) */
		req_complete(r, (long)filled);
		return;
	}
	case LXP_NETFSW_STAT:
		if (r->step == 0) { /* Rwalk: walked to the temp fid (or a component missing) */
			uint16_t nwqid = get16(body, &o);
			const char *cp[NETFS_MAXWELEM];
			size_t cl[NETFS_MAXWELEM];
			int n = path_split(r->path, cp, cl, NETFS_MAXWELEM);
			if (n > 0 && nwqid < n) { /* partial walk: the temp fid was not bound */
				r->fid = -1;
				req_complete(r, -LXP_ENOENT);
				return;
			}
			r->step = 1;
			return; /* stay inflight; pump rebuilds Tlgetattr */
		}
		if (r->step == 1) { /* Rlgetattr → fill guest stat, then clunk */
			uint32_t mode;
			uint64_t size, mtime, ino;
			parse_getattr(body, o, &mode, &size, &mtime, &ino);
			r->result = lxp_netfs_fill_stat(r->owner, r->ubuf, r->statkind, mode, size,
							    mtime, ino);
			r->step = 2; /* send Tclunk(fid) */
			return;
		}
		if (r->step == 2) { /* Rclunk → done */
			fid_free(r->fid);
			r->fid = -1;
			req_complete(r, r->result);
			return;
		}
		break;

#if defined(LXP_ENABLE_NETFS_EXEC)
	case LXP_NETFSW_EXECFETCH:
		if (r->step == 0) { /* Rwalk */
			uint16_t nwqid = get16(body, &o);
			const char *cp[NETFS_MAXWELEM];
			size_t cl[NETFS_MAXWELEM];
			int n = path_split(r->path, cp, cl, NETFS_MAXWELEM);
			if (n > 0 && nwqid < n) {
				r->fid = -1;
				req_complete(r, -LXP_ENOENT);
				return;
			}
			r->step = 1;
			return;
		}
		if (r->step == 1) { /* Rlgetattr → capture the file size (regular files only) */
			uint32_t mode;
			uint64_t size, mtime, ino;
			parse_getattr(body, o, &mode, &size, &mtime, &ino);
			if ((mode & LXP_S_IFMT) != LXP_S_IFREG) {
				r->result = -LXP_EACCES;
				r->step = 4; /* skip open/read → just clunk the walked fid */
				return;
			}
			r->ulen = (size_t)size; /* total bytes to fetch */
			r->step = 2;
			return;
		}
		if (r->step == 2) { /* Rlopen → begin chunked reads */
			r->off = 0;
			g_exec_size = 0;
			r->step = 3;
			return;
		}
		if (r->step == 3) { /* Rread → copy a chunk into staging */
			uint32_t cnt = get32(body, &o);
			if (cnt > blen - 4)
				cnt = (uint32_t)(blen - 4);
			if (r->off + cnt > g_exec_cap)
				cnt = (uint32_t)(g_exec_cap - r->off);
			if (cnt)
				memcpy(g_exec_buf + r->off, body + o, cnt);
			r->off += cnt;
			g_exec_size = r->off;
			int eof = (cnt == 0) || (r->off >= r->ulen);
			int full = (r->off >= g_exec_cap);
			if (full && !eof)
				r->result = -LXP_ENOEXEC; /* image exceeds the staging buffer */
			if (eof || full)
				r->step = 4; /* clunk */
			return;		     /* else stay step 3 and read more */
		}
		if (r->step == 4) { /* Rclunk → done (result 0, or an error latched above) */
			fid_free(r->fid);
			r->fid = -1;
			req_complete(r, r->result);
			return;
		}
		break;
#endif

	default: /* REQ_OP_CLUNK: Rclunk */
		fid_free(r->fid);
		r->state = REQ_FREE;
		g_inflight = -1;
		return;
	}
	/* Unexpected reply for the step: fail the op. */
	req_complete(r, -LXP_EIO);
}

/* ---- the pump: advance the transport one step ------------------------------ */
static void pump(uint64_t now_us)
{
	if (g_conn != CONN_UP) {
		conn_connect(now_us);
		if (g_conn != CONN_UP)
			return;
	}

	/* Flush any pending outgoing message. */
	int f = tx_flush();
	if (f < 0) {
		conn_drop();
		return;
	}
	if (f == 0)
		return; /* still sending; come back next pass */

	/* Drain replies for the in-flight request. */
	if (g_inflight >= 0) {
		int rf = rx_fill();
		if (rf < 0) {
			conn_drop();
			return;
		}
		for (;;) {
			size_t len = rx_message();
			if (len == (size_t)-1) {
				conn_drop();
				return;
			}
			if (!len)
				break;
			struct netfs_req *r = &g_req[g_inflight];
			uint8_t type = g_rx[4];
			handle_reply(r, type, g_rx + 7, len - 7);
			rx_consume(len);
			/* If the reply advanced a step (still INFLIGHT), rebuild + send it. */
			if (r->state == REQ_INFLIGHT && g_inflight >= 0) {
				long e = req_build(r);
				if (e < 0) {
					req_complete(r, e);
				} else {
					if (tx_flush() < 0) {
						conn_drop();
						return;
					}
				}
			}
			if (g_inflight < 0)
				break;
		}
	}

	/* Start the next request: prefer a queued guest request (lowest seq), else a
	 * background clunk. */
	if (g_inflight < 0 && g_txoff == g_txlen) {
		int best = -1;
		uint32_t bseq = 0xffffffffu;
		for (int i = 0; i < NETFS_NREQ; i++)
			if (g_req[i].state == REQ_QUEUED && g_req[i].seq < bseq) {
				bseq = g_req[i].seq;
				best = i;
			}
		if (best >= 0) {
			struct netfs_req *r = &g_req[best];
			long e = req_build(r);
			if (e < 0) {
				req_complete(r, e);
			} else {
				r->state = REQ_INFLIGHT;
				g_inflight = best;
				if (tx_flush() < 0) {
					conn_drop();
					return;
				}
			}
		} else if (g_clunk_head != g_clunk_tail) {
			/* fire a background clunk through a throwaway internal request */
			for (int i = 0; i < NETFS_NREQ; i++)
				if (g_req[i].state == REQ_FREE) {
					struct netfs_req *r = &g_req[i];
					memset(r, 0, sizeof(*r));
					r->op = REQ_OP_CLUNK;
					r->owner = NULL;
					r->fid = g_clunk_fid[g_clunk_head];
					g_clunk_head = (g_clunk_head + 1) % NETFS_NCLUNK;
					r->state = REQ_INFLIGHT;
					g_inflight = i;
					if (req_build(r) == 0 && tx_flush() < 0)
						conn_drop();
					break;
				}
		}
	}
}

/* ---- request allocation + submit ------------------------------------------- */
static struct netfs_req *req_new(lxp_proc_t *p, uint8_t op)
{
	for (int i = 0; i < NETFS_NREQ; i++)
		if (g_req[i].state == REQ_FREE) {
			struct netfs_req *r = &g_req[i];
			memset(r, 0, sizeof(*r));
			r->state = REQ_QUEUED;
			r->op = op;
			r->owner = p;
			r->oi = -1;
			r->fid = -1;
			r->seq = g_req_seq++;
			p->netfs_req = i;
			p->netfs_wait = op;
			p->netfs_deadline_us = 0;
			return r;
		}
	return NULL;
}

/* ---- mount config + init --------------------------------------------------- */
void lxp_netfs_mount_config(const char *mp, const uint8_t ip[4], uint16_t port,
				const char *aname, const char *uname)
{
	memset(&g_mnt, 0, sizeof(g_mnt));
	if (mp) {
		size_t n = strlen(mp);
		if (n >= sizeof(g_mnt.mp))
			n = sizeof(g_mnt.mp) - 1;
		memcpy(g_mnt.mp, mp, n);
		g_mnt.mplen = n;
	}
	if (ip)
		memcpy(g_mnt.ip, ip, 4);
	g_mnt.port = port;
	if (aname) {
		size_t n = strlen(aname);
		if (n >= sizeof(g_mnt.aname))
			n = sizeof(g_mnt.aname) - 1;
		memcpy(g_mnt.aname, aname, n);
	}
	{
		const char *u = (uname && uname[0]) ? uname : "root";
		size_t n = strlen(u);
		if (n >= sizeof(g_mnt.uname))
			n = sizeof(g_mnt.uname) - 1;
		memcpy(g_mnt.uname, u, n);
	}
	g_mnt.configured = 1;
}

void lxp_netfs_init(void)
{
	uint64_t now = 0;
	lxp_time_us(&now);
	g_reconnect_at_us = 0;
	conn_connect(now); /* best-effort; a down server reconnects lazily */
}

/* ---- provider entry points (called from the syscall handlers) -------------- */
int lxp_netfs_lookup(const char *abspath)
{
	if (!g_mnt.configured || !g_mnt.mplen)
		return -1;
	if (strncmp(abspath, g_mnt.mp, g_mnt.mplen) != 0)
		return -1;
	char c = abspath[g_mnt.mplen];
	if (c == '\0' || c == '/')
		return 0; /* the mount point itself or something under it */
	return -1;
}

/* Return the mount-relative remote path ("/" for the mount root). */
static const char *relpath(const char *abspath)
{
	const char *rp = abspath + g_mnt.mplen;
	return (*rp == '\0') ? "/" : rp;
}

long lxp_netfs_open(lxp_proc_t *p, const char *abspath, int flags)
{
	if (flags & (LXP_O_WRONLY | LXP_O_RDWR | LXP_O_CREAT | LXP_O_TRUNC))
		return -LXP_EROFS;
	const char *rp = relpath(abspath);
	if (strlen(rp) >= LXP_PATH_MAX)
		return -LXP_ENAMETOOLONG;
	int oi = -1;
	for (int i = 0; i < NETFS_NOPEN; i++)
		if (!g_open[i].used) {
			oi = i;
			break;
		}
	if (oi < 0)
		return -LXP_EMFILE;
	struct netfs_req *r = req_new(p, LXP_NETFSW_OPEN);
	if (!r)
		return -LXP_EMFILE;
	memset(&g_open[oi], 0, sizeof(g_open[oi]));
	g_open[oi].used = 1; /* reserve; finalized on Rlopen (or freed on error) */
	r->oi = oi;
	r->flags = flags;
	strcpy(r->path, rp);
	p->netfs_oi = oi;
	return 0; /* parked */
}

long lxp_netfs_read(lxp_proc_t *p, int oi, void *ubuf, size_t len)
{
	struct netfs_open *op = open_slot(oi);
	if (!op)
		return -LXP_EBADF;
	if (op->stale)
		return -LXP_ESTALE;
	if (op->is_dir)
		return -LXP_EISDIR;
	if (len == 0)
		return 0;
	if (!user_ok(p, ubuf, len, 1))
		return -LXP_EFAULT;
	struct netfs_req *r = req_new(p, LXP_NETFSW_READ);
	if (!r)
		return -LXP_EMFILE;
	r->oi = oi;
	r->ubuf = (uintptr_t)ubuf;
	r->ulen = len;
	p->netfs_oi = oi;
	return 0; /* parked */
}

/* lseek(2) on an FD_NET fd: cursor math against the shared open offset + cached size.
 * Returns the new absolute offset, or a negative Linux errno. */
long lxp_netfs_lseek(int oi, long off, int whence)
{
	struct netfs_open *op = open_slot(oi);
	if (!op)
		return -LXP_EBADF;
	if (op->is_dir)
		return -LXP_EISDIR;
	uint64_t base = (whence == LXP_SEEK_CUR)   ? op->rd_off
			: (whence == LXP_SEEK_END) ? op->size
						       : 0;
	long long np = (long long)base + off;
	if (np < 0)
		return -LXP_EINVAL;
	op->rd_off = (uint64_t)np;
	return (long)np;
}

long lxp_netfs_getdents(lxp_proc_t *p, int oi, uintptr_t ubuf, size_t cap, int is64)
{
	struct netfs_open *op = open_slot(oi);
	if (!op)
		return -LXP_EBADF;
	if (op->stale)
		return -LXP_ESTALE;
	if (!op->is_dir)
		return -LXP_ENOTDIR;
	if (!user_ok(p, (void *)ubuf, cap, 1))
		return -LXP_EFAULT;
	struct netfs_req *r = req_new(p, LXP_NETFSW_GETDENTS);
	if (!r)
		return -LXP_EMFILE;
	r->oi = oi;
	r->ubuf = ubuf;
	r->ulen = cap;
	r->is64 = is64;
	p->netfs_oi = oi;
	return 0; /* parked */
}

long lxp_netfs_stat(lxp_proc_t *p, const char *abspath, uintptr_t ustat, int statkind)
{
	const char *rp = relpath(abspath);
	if (strlen(rp) >= LXP_PATH_MAX)
		return -LXP_ENAMETOOLONG;
	struct netfs_req *r = req_new(p, LXP_NETFSW_STAT);
	if (!r)
		return -LXP_EMFILE;
	r->ubuf = ustat;
	r->statkind = statkind;
	strcpy(r->path, rp);
	p->netfs_oi = -1;
	return 0; /* parked */
}

int lxp_netfs_fstat(int oi, uint32_t *mode, uint64_t *size, uint64_t *mtime, uint64_t *ino)
{
	struct netfs_open *op = open_slot(oi);
	if (!op)
		return -1;
	if (mode)
		*mode = op->mode;
	if (size)
		*size = op->size;
	if (mtime)
		*mtime = op->mtime;
	if (ino)
		*ino = op->ino;
	return 0;
}

void lxp_netfs_get(int oi)
{
	struct netfs_open *op = open_slot(oi);
	if (op && op->refs < 0xff)
		op->refs++;
}

void lxp_netfs_close(int oi)
{
	struct netfs_open *op = open_slot(oi);
	if (!op)
		return;
	if (op->refs > 1) {
		op->refs--;
		return;
	}
	if (!op->stale && op->fid > 0)
		clunk_enqueue(op->fid);
	else
		fid_free(op->fid);
	op->used = 0;
}

#if defined(LXP_ENABLE_NETFS_EXEC)
/* ---- exec off the mount: fetch the whole ELF into the engine staging buffer ---- */
long lxp_netfs_exec_fetch(lxp_proc_t *p, const char *abspath)
{
	const char *rp = relpath(abspath);
	if (strlen(rp) >= LXP_PATH_MAX)
		return -LXP_ENAMETOOLONG;
	size_t cap = 0;
	g_exec_buf = lxp_netfs_exec_stage(&cap);
	if (!g_exec_buf || cap == 0)
		return -LXP_ENOMEM; /* no staging buffer on this build */
	g_exec_cap = cap;
	g_exec_size = 0;
	struct netfs_req *r = req_new(p, LXP_NETFSW_EXECFETCH);
	if (!r)
		return -LXP_EMFILE;
	strcpy(r->path, rp);
	r->off = 0;
	p->netfs_oi = -1;
	return 0; /* parked */
}

const uint8_t *lxp_netfs_exec_image(size_t *size)
{
	if (size)
		*size = g_exec_size;
	return g_exec_buf;
}
#endif /* LXP_ENABLE_NETFS_EXEC */

/* ---- coordinator: retry a parked op / periodic pump ------------------------ */
long lxp_netfs_retry(lxp_proc_t *p)
{
	uint64_t now = 0;
	lxp_time_us(&now);
	pump(now);

	int ri = p->netfs_req;
	if (ri < 0 || ri >= NETFS_NREQ)
		return -LXP_EBADF;
	struct netfs_req *r = &g_req[ri];
	if (r->state != REQ_DONE)
		return -LXP_EAGAIN; /* still in flight */

	long result = r->result;
	uint8_t op = r->op;
	int oi = r->oi;
	r->state = REQ_FREE;
	p->netfs_req = -1;

	if (op == LXP_NETFSW_OPEN && result >= 0) {
		int fd = lxp_fd_install(p, LXP_FD_NET, oi);
		if (fd < 0) {
			lxp_netfs_close(oi);
			return -LXP_EMFILE;
		}
		return fd;
	}
#if defined(LXP_ENABLE_NETFS_EXEC)
	if (op == LXP_NETFSW_EXECFETCH && result >= 0) {
		/* the ELF is staged: flag the exec so the run loop's EV_EXEC launches it from the
		 * staging buffer. Returning 0 with exec_pending set tells the run loop NOT to resume. */
		p->exec_pending = 1;
		p->exec_file_idx = LXP_NETFS_EXEC_SENTINEL;
		return 0;
	}
#endif
	return result;
}

void lxp_netfs_tick(uint64_t now_us)
{
	/* Keep the transport moving (background clunks + reconnect) even with no parked proc. */
	pump(now_us);
}

int lxp_netfs_busy(void)
{
	if (g_inflight >= 0 || g_clunk_head != g_clunk_tail)
		return 1;
	for (int i = 0; i < NETFS_NREQ; i++)
		if (g_req[i].state == REQ_QUEUED)
			return 1;
	return 0;
}

/* ---- fork / exit fd lifecycle ---------------------------------------------- */
void lxp_netfs_fork_inherit(lxp_proc_t *child)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (child->fds[fd].kind == LXP_FD_NET)
			lxp_netfs_get(child->fds[fd].file_idx);
}

void lxp_netfs_proc_exit(lxp_proc_t *p)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (p->fds[fd].kind == LXP_FD_NET) {
			lxp_netfs_close(p->fds[fd].file_idx);
			p->fds[fd].kind = 0; /* FD_FREE */
		}
}

#endif /* LXP_ENABLE_NETFS */
