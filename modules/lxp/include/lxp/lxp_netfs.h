/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_NETFS_H
#define OVE_LINUX_NETFS_H

/**
 * @file netfs.h
 * @defgroup ove_linux_netfs Linux personality remote filesystem (9P2000.L)
 * @ingroup ove_linux
 * @brief A read-only remote filesystem mounted under a path (e.g. /mnt/pi).
 *
 * A single mount over a coordinator-owned, non-blocking TCP connection to a 9P
 * server (diod on a Raspberry Pi). The guest browses it transparently — the
 * FD_NET branches of the syscall handlers route open/read/lseek/close/stat/
 * getdents on a /mnt/pi path to this layer, which speaks 9P2000.L and returns
 * Linux-ABI results. It mirrors the socket layer (linux/net/ove_linux_net.c): a
 * refcounted per-open pool (each = a 9P fid + cursor), fork/dup share an open,
 * and the last close clunks the fid.
 *
 * Blocking model: every remote-touching op needs a Pi round-trip and the syscall
 * handlers run in the SVC/exception context, so nothing blocks inline. An op
 * submits a 9P request, sets proc->netfs_wait, and returns 0 (parked); the
 * run-loop coordinator pumps the transport each pass via lxp_netfs_retry and
 * resumes the guest via spawn_resume(...,result). Ops answerable from cached
 * open-state (fstat, lseek) run inline. The transport is serialized: one 9P
 * request in flight, a FIFO of the rest.
 *
 * @note Requires @c CONFIG_OVE_LINUX_NETFS.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/** proc->netfs_wait op codes: which parked netfs op the coordinator retries.
 *  Shared with the run loop (backends/common/lxp_run.c). */
#define LXP_NETFSW_OPEN 1u	  /**< open: Twalk -> Tlgetattr -> Tlopen -> install fd. */
#define LXP_NETFSW_READ 2u	  /**< read: Tread -> copy to guest. */
#define LXP_NETFSW_GETDENTS 3u /**< getdents64: Treaddir -> emit dirent64 records. */
#define LXP_NETFSW_STAT 4u	  /**< path stat: Twalk -> Tlgetattr -> Tclunk -> fill guest stat. */
#define LXP_NETFSW_EXECFETCH 5u /**< Phase B: read a whole remote ELF into the exec staging buffer. */

/* ---- boot: mount config + connection init (coordinator thread) ------------- */

/**
 * @brief Configure the static mount (call at app boot, before lxp_run).
 * @param mountpoint absolute guest path, e.g. "/mnt/pi" (copied; must outlive use).
 * @param ip         IPv4 of the 9P server (the Pi), e.g. {172,1,1,1}.
 * @param port       9P server TCP port (diod default 564; a non-privileged port otherwise).
 * @param aname      the export path to attach (diod -e path, e.g. "/srv/pi9").
 * @param uname      the user name to attach as (diod -n no-auth accepts any; "root").
 */
void lxp_netfs_mount_config(const char *mountpoint, const uint8_t ip[4], uint16_t port,
				const char *aname, const char *uname);

/** @brief Open the socket + do the blocking Tversion/Tattach handshake. Coordinator
 *  thread only (from lxp_dev_autoreg_all's region). A down server is non-fatal:
 *  the mount is marked DISCONNECTED and reconnected lazily; boot never hangs. */
void lxp_netfs_init(void);

/* ---- syscall-layer <-> netfs-core interface (called from ove_linux_syscall.c) ---- */

/** @return mount id (>=0) if @p abspath is at or under the mount point, else -1. */
int lxp_netfs_lookup(const char *abspath);

/** open(2): submit Twalk->Tlgetattr->Tlopen for the /mnt path; parks (returns 0 with
 *  netfs_wait=NETFSW_OPEN), or a negative Linux errno inline (path too long, no mount). */
long lxp_netfs_open(lxp_proc_t *p, const char *abspath, int flags);

/** read(2)/pread(2): submit Tread at @p off (SIZE_MAX off => use the fd cursor); parks. */
long lxp_netfs_read(lxp_proc_t *p, int oi, void *ubuf, size_t len);

/** getdents64(2): submit Treaddir from the open's dir cursor; parks. @p is64 selects the
 *  64-bit dirent layout (getdents64) vs the 32-bit one (getdents). */
long lxp_netfs_getdents(lxp_proc_t *p, int oi, uintptr_t ubuf, size_t cap, int is64);

/** stat/lstat/fstatat/statx on a /mnt path: submit Twalk->Tlgetattr->Tclunk; parks. On
 *  completion the retry marshals the attrs into the guest buffer via lxp_netfs_fill_stat.
 *  @p statkind: 0 = stat64/newfstatat kstat, 1 = statx. */
long lxp_netfs_stat(lxp_proc_t *p, const char *abspath, uintptr_t ustat, int statkind);

/** Cached open attributes for fstat/statx(fd) + lseek(SEEK_END) — no round-trip.
 *  Fills any non-NULL out param. @return 0, or -1 if @p oi is not a live open. */
int lxp_netfs_fstat(int oi, uint32_t *mode, uint64_t *size, uint64_t *mtime, uint64_t *ino);

/** lseek(2) on an FD_NET fd: cursor math against the shared open offset + cached size.
 *  No round-trip. @return the new absolute offset, or a negative Linux errno. */
long lxp_netfs_lseek(int oi, long off, int whence);

/** Add a reference on open @p oi (dup/fork inheritance). */
void lxp_netfs_get(int oi);
/** Drop a reference on open @p oi (close/exit); the last ref enqueues a background Tclunk.
 *  Never parks — close(2) always completes at once. */
void lxp_netfs_close(int oi);

/** Retry a parked netfs op for the coordinator: pump the transport once, advance the
 *  in-flight request, and return the completed Linux-ABI result or -LXP_EAGAIN. */
long lxp_netfs_retry(lxp_proc_t *p);

/** fork: the child inherited the parent's FD_NET fds — add a reference to each. */
void lxp_netfs_fork_inherit(lxp_proc_t *child);
/** exit: release every FD_NET open the process still holds (enqueues clunks). */
void lxp_netfs_proc_exit(lxp_proc_t *p);

/* ---- run-loop <-> netfs interface ------------------------------------------ */

/** Coordinator periodic work: pump the transport (drain background clunks, service the
 *  reconnect backoff) even when no proc is parked. Called from the run loop each pass. */
void lxp_netfs_tick(uint64_t now_us);

/** 1 if any netfs request is outstanding (so the run loop holds its ≤5 ms retry tick). */
int lxp_netfs_busy(void);

/* ---- implemented in ove_linux_syscall.c, called by the netfs retry --------- */

/** Marshal remote attributes into the guest's stat/statx buffer (the netfs retry owns the
 *  9P transport; the syscall TU owns the kstat/statx layout + user_ok). @return 0 or -errno. */
long lxp_netfs_fill_stat(lxp_proc_t *p, uintptr_t ustat, int statkind, uint32_t mode,
			     uint64_t size, uint64_t mtime, uint64_t ino);

/** access_ok for the netfs handlers to validate a guest pointer (confused-deputy guard —
 *  handlers run PRIVILEGED). Defined in ove_linux_syscall.c. */
int user_ok(const lxp_proc_t *p, const void *ptr, size_t len, int write);

/* ---- Phase B: exec a program off the mount (CONFIG_OVE_LINUX_NETFS_EXEC) ---- */
#if defined(CONFIG_OVE_LINUX_NETFS_EXEC)
/** exec_file_idx marker: the image to launch lives in the netfs exec staging buffer (RAM),
 *  not the rootfs table. The run loop's EV_EXEC sources it via lxp_netfs_exec_image. */
#define LXP_NETFS_EXEC_SENTINEL (-2)

/** execve of a /mnt path: submit walk/getattr/open + chained Tread of the whole ELF into the
 *  staging buffer; parks (netfs_wait=NETFSW_EXECFETCH). On completion the retry sets the proc's
 *  exec_pending + exec_file_idx=SENTINEL and the run loop launches from the staged image.
 *  Returns 0 (parked) or a negative Linux errno inline. */
long lxp_netfs_exec_fetch(lxp_proc_t *p, const char *abspath);
/** The staged remote ELF after a completed EXECFETCH: bytes + size for launch(). */
const uint8_t *lxp_netfs_exec_image(size_t *size);

/** Engine-provided RAM staging buffer for a fetched remote ELF (SDRAM on the STM32, a static
 *  buffer under test). Returns the buffer + its capacity in @p cap, or NULL if unavailable.
 *  Provided by the backend/board (gated on CONFIG_OVE_LINUX_NETFS_EXEC). */
uint8_t *lxp_netfs_exec_stage(size_t *cap);
#endif

/** Wake the coordinator so it retries parked netfs I/O at once (the eth RX path calls this
 *  after delivering frames — a 9P reply may have arrived). Defined by the run loop; weak
 *  no-op otherwise. */
void lxp_netfs_kick(void);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LINUX_NETFS_H */
