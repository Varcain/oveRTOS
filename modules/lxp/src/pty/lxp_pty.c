/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pseudo-terminal (Unix98 pty) layer for the Linux personality.
 *
 * A pty is two in-memory rings — m2s (master→slave, program INPUT) and s2m
 * (slave→master, program OUTPUT) — plus a minimal in-kernel line discipline. It is
 * the two-ended pipe with a transform at the boundary: master WRITE runs input
 * processing (ICRNL, ISIG ^C→SIGINT, canonical line editing + echo), slave WRITE runs
 * output processing (OPOST/ONLCR). Lifecycle is recompute-open-ends (like the pipe):
 * no per-fd refcount, so close/dup/fork/exit need no pty hook — a scan of every live
 * proc's fd table yields the master/slave open counts on demand (drives EOF/hangup).
 *
 * dropbear opens /dev/ptmx (master) + /dev/pts/N (slave = the login shell's ctty) and
 * shuttles bytes between the master and the SSH channel; ash reads/writes the slave.
 * Gated on CONFIG_OVE_LINUX_PTY.
 */
#include "lxp/lxp_pty.h"

#if defined(CONFIG_OVE_LINUX_PTY)

#include <string.h>

#include "lxp/lxp_dev.h" /* user_ok() (confused-deputy guard for ioctl arg pointers) */

/* One pty pair = 2 concurrent SSH logins' worth on this tier (each login holds a
 * master + a slave). Rings are small — a terminal is interactive, not bulk; the s2m
 * output ring applies backpressure (the shell's write parks) when the server is slow
 * to drain, so a modest ring never drops output. Zephyr's per-program K_USER domains
 * eat SRAM, so halve it there (same rationale as the pipe ring). */
#define LXP_NPTY 2
/* Rings are small — a login terminal is interactive, not bulk. The s2m OUTPUT ring
 * applies backpressure (the shell's write parks) when the server is slow to drain, so a
 * burst is paced, never dropped. 1 KB keeps 2 ptys' .bss (~4.6 KB) inside internal SRAM
 * (the guest program/heap pools already own the external SDRAM); Zephyr's per-program
 * K_USER domains eat SRAM, so halve it there. */
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#define LXP_PTY_BUF 512
#else
#define LXP_PTY_BUF 1024
#endif
#define LXP_PTY_CANON 256 /* max in-progress canonical line before it must end */

typedef struct {
	uint8_t buf[LXP_PTY_BUF];
	size_t r, w, n; /* ring read/write index [0,BUF) + bytes buffered */
} pty_ring_t;

typedef struct {
	pty_ring_t m2s; /* master→slave: program input (keystrokes, post-discipline) */
	pty_ring_t s2m; /* slave→master: program output (+ canonical echo) */
	uint8_t canon[LXP_PTY_CANON]; /* in-progress canonical line (not yet readable) */
	size_t canon_n;
	lxp_termios tio; /* line-discipline state (TCGETS/TCSETS) */
	lxp_winsize ws;
	int fg_pgrp; /* TIOCSPGRP foreground group (advisory; ^C broadcasts to slave holders) */
	int used;
	int locked;  /* TIOCSPTLCK: slave locked until unlockpt (advisory here) */
	int m2s_eof; /* one-shot EOF pending on the slave (^D on an empty canonical line) */
	uint8_t m_nb, s_nb; /* O_NONBLOCK per end: an empty/full ring returns EAGAIN, not park.
			     * dropbear sets the master non-blocking and drives it with select. */
} lxp_pty_t;

static lxp_pty_t g_ptys[LXP_NPTY];

/* ── ring helpers ─────────────────────────────────────────────── */

static size_t ring_space(const pty_ring_t *r)
{
	return LXP_PTY_BUF - r->n;
}
static void ring_putc(pty_ring_t *r, uint8_t c)
{
	if (r->n < LXP_PTY_BUF) {
		r->buf[r->w] = c;
		r->w = (r->w + 1) % LXP_PTY_BUF;
		r->n++;
	}
}
static size_t ring_read(pty_ring_t *r, uint8_t *out, size_t len)
{
	if (len > r->n)
		len = r->n;
	for (size_t i = 0; i < len; i++) {
		out[i] = r->buf[r->r];
		r->r = (r->r + 1) % LXP_PTY_BUF;
	}
	r->n -= len;
	return len;
}
/* Canonical read: up to and INCLUDING the first newline (one line per read). */
static size_t ring_read_line(pty_ring_t *r, uint8_t *out, size_t len)
{
	size_t i = 0;
	while (i < len && r->n > 0) {
		uint8_t c = r->buf[r->r];
		r->r = (r->r + 1) % LXP_PTY_BUF;
		r->n--;
		out[i++] = c;
		if (c == '\n')
			break;
	}
	return i;
}

/* ── open-end counting (recompute, no refcount — mirrors pipe_ends) ─── */

static void pty_ends(int idx, int *masters, int *slaves)
{
	*masters = 0;
	*slaves = 0;
	lxp_proc_t *tab = lxp_proc_table();
	int n = lxp_proc_nslot();
	if (!tab)
		return;
	for (int s = 0; s < n; s++) {
		if (!tab[s].alive)
			continue;
		for (int fd = 0; fd < LXP_MAX_FDS; fd++)
			if (tab[s].fds[fd].kind == LXP_FD_PTY && tab[s].fds[fd].file_idx == idx)
				(tab[s].fds[fd].rw ? (*masters)++ : (*slaves)++);
	}
}

/* ^C (and friends): deliver @p sig to every live proc holding this pty's SLAVE end —
 * the shell (which catches SIGINT and re-prompts) plus its foreground child (default
 * action = die). Process-group narrowing is a future refinement (this tier does not
 * track pgid — setpgid is inert), and broadcasting is safe because the shell survives. */
static void pty_signal_slaves(int idx, int sig)
{
	lxp_proc_t *tab = lxp_proc_table();
	int n = lxp_proc_nslot();
	if (!tab)
		return;
	for (int s = 0; s < n; s++) {
		if (!tab[s].alive)
			continue;
		for (int fd = 0; fd < LXP_MAX_FDS; fd++)
			if (tab[s].fds[fd].kind == LXP_FD_PTY &&
			    tab[s].fds[fd].file_idx == idx && tab[s].fds[fd].rw == 0) {
				tab[s].pending_sig = sig;
				break; /* one delivery per proc */
			}
	}
}

/* ── line discipline ──────────────────────────────────────────── */

/* Master WRITE → toward the slave (program input). Returns bytes consumed (>0), or
 * -EAGAIN if nothing fit (the m2s ring is full and the shell has not drained it). */
static long pty_input(int idx, const uint8_t *in, size_t len)
{
	lxp_pty_t *pt = &g_ptys[idx];
	int icanon = pt->tio.c_lflag & LXP_ICANON;
	int echo = pt->tio.c_lflag & LXP_ECHO;
	int isig = pt->tio.c_lflag & LXP_ISIG;
	int icrnl = pt->tio.c_iflag & LXP_ICRNL;
	size_t consumed = 0;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = in[i];
		if (icrnl && c == '\r')
			c = '\n';
		if (isig && c == pt->tio.c_cc[LXP_VINTR]) {
			pty_signal_slaves(idx, LXP_SIGINT);
			consumed++;
			continue;
		}
		if (!icanon) {
			/* Raw mode: the shell's own line editor reads byte-by-byte and echoes
			 * itself, so pass through untouched (echo only if the caller left it on). */
			if (ring_space(&pt->m2s) == 0)
				return consumed ? (long)consumed : -LXP_EAGAIN;
			ring_putc(&pt->m2s, c);
			if (echo)
				ring_putc(&pt->s2m, c);
			consumed++;
			continue;
		}
		/* Canonical mode: accumulate a line; deliver it whole on newline. */
		if (c == pt->tio.c_cc[LXP_VERASE] || c == 0x08) {
			if (pt->canon_n > 0) {
				pt->canon_n--;
				if (echo) { /* erase the echoed char: back, space, back */
					ring_putc(&pt->s2m, 0x08);
					ring_putc(&pt->s2m, ' ');
					ring_putc(&pt->s2m, 0x08);
				}
			}
			consumed++;
			continue;
		}
		if (c == '\n') {
			if (ring_space(&pt->m2s) < pt->canon_n + 1)
				return consumed ? (long)consumed : -LXP_EAGAIN;
			for (size_t k = 0; k < pt->canon_n; k++)
				ring_putc(&pt->m2s, pt->canon[k]);
			ring_putc(&pt->m2s, '\n');
			pt->canon_n = 0;
			if (echo) {
				ring_putc(&pt->s2m, '\r');
				ring_putc(&pt->s2m, '\n');
			}
			consumed++;
			continue;
		}
		if (c == pt->tio.c_cc[LXP_VEOF]) { /* ^D: flush the line; empty → EOF */
			if (ring_space(&pt->m2s) < pt->canon_n)
				return consumed ? (long)consumed : -LXP_EAGAIN;
			for (size_t k = 0; k < pt->canon_n; k++)
				ring_putc(&pt->m2s, pt->canon[k]);
			if (pt->canon_n == 0)
				pt->m2s_eof = 1; /* ^D on an empty line = end-of-file */
			pt->canon_n = 0;
			consumed++;
			continue;
		}
		if (pt->canon_n < LXP_PTY_CANON) {
			pt->canon[pt->canon_n++] = c;
			if (echo)
				ring_putc(&pt->s2m, c);
		}
		consumed++;
	}
	return (long)consumed;
}

/* Slave WRITE → toward the master (program output). ONLCR maps \n → \r\n. Returns
 * bytes consumed (>0), or -EAGAIN if the s2m ring is full (server not draining). */
static long pty_output(int idx, const uint8_t *in, size_t len)
{
	lxp_pty_t *pt = &g_ptys[idx];
	int opost = pt->tio.c_oflag & LXP_OPOST;
	int onlcr = pt->tio.c_oflag & LXP_ONLCR;
	size_t consumed = 0;
	for (size_t i = 0; i < len; i++) {
		uint8_t c = in[i];
		if (opost && onlcr && c == '\n') {
			if (ring_space(&pt->s2m) < 2)
				return consumed ? (long)consumed : -LXP_EAGAIN;
			ring_putc(&pt->s2m, '\r');
			ring_putc(&pt->s2m, '\n');
		} else {
			if (ring_space(&pt->s2m) == 0)
				return consumed ? (long)consumed : -LXP_EAGAIN;
			ring_putc(&pt->s2m, c);
		}
		consumed++;
	}
	return (long)consumed;
}

/* ── public entry points ──────────────────────────────────────── */

long lxp_pty_read(lxp_proc_t *p, int idx, int is_master, void *ubuf, size_t len)
{
	(void)p;
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return -LXP_EBADF;
	lxp_pty_t *pt = &g_ptys[idx];
	if (len == 0)
		return 0;
	uint8_t *out = (uint8_t *)ubuf;
	int masters, slaves;
	if (is_master) { /* server reads program output from s2m */
		if (pt->s2m.n > 0)
			return (long)ring_read(&pt->s2m, out, len);
		pty_ends(idx, &masters, &slaves);
		if (slaves == 0)
			return 0; /* shell exited/closed the slave → EOF, server drops the channel */
		return -LXP_EAGAIN;
	}
	/* slave: the shell reads its input from m2s */
	if (pt->m2s.n > 0)
		return (long)((pt->tio.c_lflag & LXP_ICANON)
				      ? ring_read_line(&pt->m2s, out, len)
				      : ring_read(&pt->m2s, out, len));
	pty_ends(idx, &masters, &slaves);
	if (masters == 0)
		return 0; /* master closed (client disconnect) → EOF/hangup → the shell exits */
	if (pt->m2s_eof) {
		pt->m2s_eof = 0;
		return 0; /* ^D */
	}
	return -LXP_EAGAIN;
}

long lxp_pty_write(lxp_proc_t *p, int idx, int is_master, const void *ubuf, size_t len)
{
	(void)p;
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return -LXP_EBADF;
	if (len == 0)
		return 0;
	const uint8_t *in = (const uint8_t *)ubuf;
	return is_master ? pty_input(idx, in, len) : pty_output(idx, in, len);
}

long lxp_pty_ioctl(lxp_proc_t *p, int idx, int is_master, unsigned long cmd,
		       unsigned long arg)
{
	(void)is_master;
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return -LXP_EBADF;
	lxp_pty_t *pt = &g_ptys[idx];
	void *ua = (void *)(uintptr_t)arg;
	switch (cmd) {
	case LXP_TCGETS:
		if (!user_ok(p, ua, sizeof(pt->tio), 1))
			return -LXP_EFAULT;
		memcpy(ua, &pt->tio, sizeof(pt->tio));
		return 0;
	case LXP_TCSETS:
	case LXP_TCSETSW:
	case LXP_TCSETSF:
		if (!user_ok(p, ua, sizeof(pt->tio), 0))
			return -LXP_EFAULT;
		memcpy(&pt->tio, ua, sizeof(pt->tio));
		return 0;
	case LXP_TIOCGPTN:
		if (!user_ok(p, ua, sizeof(uint32_t), 1))
			return -LXP_EFAULT;
		*(uint32_t *)ua = (uint32_t)idx;
		return 0;
	case LXP_TIOCSPTLCK:
		if (!user_ok(p, ua, sizeof(int), 0))
			return -LXP_EFAULT;
		pt->locked = *(int *)ua;
		return 0;
	case LXP_TIOCGWINSZ:
		if (!user_ok(p, ua, sizeof(pt->ws), 1))
			return -LXP_EFAULT;
		memcpy(ua, &pt->ws, sizeof(pt->ws));
		return 0;
	case LXP_TIOCSWINSZ:
		if (!user_ok(p, ua, sizeof(pt->ws), 0))
			return -LXP_EFAULT;
		memcpy(&pt->ws, ua, sizeof(pt->ws));
		return 0;
	case LXP_TIOCSPGRP:
		if (!user_ok(p, ua, sizeof(int), 0))
			return -LXP_EFAULT;
		pt->fg_pgrp = *(int *)ua;
		return 0;
	case LXP_TIOCGPGRP:
		if (!user_ok(p, ua, sizeof(int), 1))
			return -LXP_EFAULT;
		*(int *)ua = pt->fg_pgrp ? pt->fg_pgrp : p->pid;
		return 0;
	case LXP_TIOCSCTTY:
	case LXP_TIOCNOTTY:
		return 0; /* the slave becomes/loses the ctty — accepted (single-session tier) */
	default:
		return -LXP_ENOTTY;
	}
}

unsigned lxp_pty_poll(int idx, int is_master)
{
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return 0;
	lxp_pty_t *pt = &g_ptys[idx];
	int masters, slaves;
	pty_ends(idx, &masters, &slaves);
	unsigned r = 0;
	if (is_master) {
		if (pt->s2m.n > 0 || slaves == 0)
			r |= LXP_POLLIN; /* data to read, or slave gone (EOF is readable) */
		if (ring_space(&pt->m2s) > 0)
			r |= LXP_POLLOUT;
	} else {
		if (pt->m2s.n > 0 || masters == 0 || pt->m2s_eof)
			r |= LXP_POLLIN;
		if (ring_space(&pt->s2m) > 0)
			r |= LXP_POLLOUT;
	}
	return r;
}

void lxp_pty_fstat(uint32_t *mode, uint64_t *size)
{
	*mode = LXP_S_IFCHR | 0620u;
	*size = 0;
}

int lxp_pty_nonblock(int idx, int is_master)
{
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return 0;
	return is_master ? g_ptys[idx].m_nb : g_ptys[idx].s_nb;
}

void lxp_pty_setfl(int idx, int is_master, int flags)
{
	if (idx < 0 || idx >= LXP_NPTY || !g_ptys[idx].used)
		return;
	uint8_t nb = (flags & LXP_O_NONBLOCK) ? 1 : 0;
	if (is_master)
		g_ptys[idx].m_nb = nb;
	else
		g_ptys[idx].s_nb = nb;
}

int lxp_pty_getfl(int idx, int is_master)
{
	int nb = lxp_pty_nonblock(idx, is_master);
	return LXP_O_RDWR | (nb ? LXP_O_NONBLOCK : 0);
}

long lxp_pty_retry(lxp_proc_t *p)
{
	switch (p->pty_wait) {
	case LXP_PTYW_SREAD:
		return lxp_pty_read(p, p->pty_idx, 0, (void *)p->pty_buf, p->pty_len);
	case LXP_PTYW_MREAD:
		return lxp_pty_read(p, p->pty_idx, 1, (void *)p->pty_buf, p->pty_len);
	case LXP_PTYW_SWRITE:
		return lxp_pty_write(p, p->pty_idx, 0, (const void *)p->pty_buf, p->pty_len);
	case LXP_PTYW_MWRITE:
		return lxp_pty_write(p, p->pty_idx, 1, (const void *)p->pty_buf, p->pty_len);
	default:
		return 0;
	}
}

/* ── open (/dev/ptmx mint, /dev/pts/N attach) ─────────────────── */

long lxp_pty_open_master(int flags)
{
	for (int i = 0; i < LXP_NPTY; i++) {
		if (g_ptys[i].used) {
			int m, s;
			pty_ends(i, &m, &s);
			if (m || s)
				continue; /* still open somewhere */
		}
		lxp_pty_t *pt = &g_ptys[i];
		memset(pt, 0, sizeof(*pt));
		pt->used = 1;
		pt->locked = 1; /* until unlockpt (TIOCSPTLCK 0) */
		pt->m_nb = (flags & LXP_O_NONBLOCK) ? 1 : 0;
		/* Default cooked termios — matches the console's TCGETS defaults. */
		pt->tio.c_iflag = LXP_ICRNL;
		pt->tio.c_oflag = LXP_OPOST | LXP_ONLCR;
		pt->tio.c_cflag = LXP_CS8 | LXP_CREAD;
		pt->tio.c_lflag = LXP_ICANON | LXP_ECHO | LXP_ISIG;
		pt->tio.c_cc[LXP_VINTR] = 3;     /* ^C */
		pt->tio.c_cc[LXP_VERASE] = 0x7f; /* DEL */
		pt->tio.c_cc[LXP_VEOF] = 4;	     /* ^D */
		pt->tio.c_cc[LXP_VMIN] = 1;
		pt->ws.ws_row = 24;
		pt->ws.ws_col = 80;
		return i;
	}
	return -LXP_EMFILE; /* pty pool exhausted */
}

long lxp_pty_open_slave(int num, int flags)
{
	if (num < 0 || num >= LXP_NPTY || !g_ptys[num].used)
		return -LXP_ENOENT;
	g_ptys[num].s_nb = (flags & LXP_O_NONBLOCK) ? 1 : 0;
	return num; /* the pool index doubles as the pts number */
}

#endif /* CONFIG_OVE_LINUX_PTY */
