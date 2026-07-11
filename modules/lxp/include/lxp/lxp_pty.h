/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file pty.h
 * @brief Pseudo-terminal (Unix98 pty) layer for the Linux personality.
 *
 * A pty is a pair of in-memory rings (master↔slave) plus a minimal in-kernel line
 * discipline (echo / canonical line editing / ICRNL / ONLCR / ISIG ^C→SIGINT),
 * modeled on the two-ended pipe (@ref lxp_pipe_retry) — recompute-open-ends
 * lifecycle, no per-fd refcount. An SSH server (dropbear) opens @c /dev/ptmx (the
 * master) + @c /dev/pts/N (the slave, the login shell's controlling tty) and shuttles
 * bytes between the master and the SSH channel; the shell reads/writes the slave.
 *
 * An FD_PTY fd carries @c file_idx = pty-pool index, @c rw = 1 master / 0 slave.
 * Gated on @c CONFIG_OVE_LINUX_PTY.
 */
#ifndef OVE_LINUX_PTY_H
#define OVE_LINUX_PTY_H

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_syscall.h"

/* Parked-op codes stored in lxp_proc.pty_wait (the coordinator retries via
 * lxp_pty_retry, mirroring pipe_wait). */
#define LXP_PTYW_SREAD 1  /* slave  read  (shell reads input)  — m2s empty, master open */
#define LXP_PTYW_MREAD 2  /* master read  (server reads output) — s2m empty, slave open */
#define LXP_PTYW_SWRITE 3 /* slave  write (shell output)       — s2m full,  master open */
#define LXP_PTYW_MWRITE 4 /* master write (server input)       — m2s full,  slave open */

/** Open @c /dev/ptmx: mint a new pty pair; returns the pool index (the master fd's
 *  @c file_idx) or a negative errno. @p flags carries O_NONBLOCK. */
long lxp_pty_open_master(int flags);

/** Open @c /dev/pts/N: validate pty number @p num and return its pool index (the slave
 *  fd's @c file_idx) or a negative errno. @p flags carries O_NONBLOCK. */
long lxp_pty_open_slave(int num, int flags);

/** read(2) routing for an FD_PTY fd (@p is_master = fd.rw). Bytes read, 0 (EOF), or
 *  @c -LXP_EAGAIN when the ring is empty and the peer end is still open (park). */
long lxp_pty_read(lxp_proc_t *p, int idx, int is_master, void *ubuf, size_t len);

/** write(2) routing for an FD_PTY fd. Bytes consumed, or @c -LXP_EAGAIN when the
 *  destination ring is full and the peer end is open (park for backpressure). */
long lxp_pty_write(lxp_proc_t *p, int idx, int is_master, const void *ubuf, size_t len);

/** ioctl(2) on a pty: TCGETS/TCSETS(W/F), TIOCGPTN, TIOCSPTLCK, TIOC[GS]WINSZ,
 *  TIOC[GS]PGRP, TIOCSCTTY/TIOCNOTTY. */
long lxp_pty_ioctl(lxp_proc_t *p, int idx, int is_master, unsigned long cmd,
		       unsigned long arg);

/** poll(2) readiness bitmap (POLLIN/POLLOUT) for an FD_PTY fd. */
unsigned lxp_pty_poll(int idx, int is_master);

/** True if this pty end is O_NONBLOCK (an empty/full ring returns EAGAIN, not park). */
int lxp_pty_nonblock(int idx, int is_master);
/** fcntl F_SETFL / F_GETFL for an FD_PTY end (tracks O_NONBLOCK). */
void lxp_pty_setfl(int idx, int is_master, int flags);
int lxp_pty_getfl(int idx, int is_master);

/** fstat(2): report a character device (S_IFCHR) with zero size. */
void lxp_pty_fstat(uint32_t *mode, uint64_t *size);

/** Retry a parked pty read/write for the run-loop coordinator; bytes / 0 (EOF) /
 *  @c -LXP_EAGAIN while still blocked. */
long lxp_pty_retry(lxp_proc_t *p);

#endif /* OVE_LINUX_PTY_H */
