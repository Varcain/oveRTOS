/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_SYSCALL_H
#define OVE_LINUX_SYSCALL_H

/**
 * @file syscall.h
 * @defgroup ove_linux Linux personality
 * @ingroup ove_mem
 * @brief Linux syscall dispatch for loaded bFLT/FDPIC programs.
 *
 * The engine-agnostic core of the oveRTOS Linux personality: it impersonates
 * the Linux kernel's syscall ABI for stock uClibc-ng binaries. A per-engine
 * SVC trap (e.g. @c backends/nuttx/nuttx_lnx_trap.c) decodes the trap frame and
 * calls @c ove_lnx_syscall(); this layer translates the call into oveRTOS
 * primitives and bounded process state. It neither installs the trap nor
 * touches memory protection — that is the engine seam's job.
 *
 * Scope (Phase A start): a minimal syscall set — @c write / @c writev /
 * @c read / @c brk / @c exit / @c exit_group — backed by a caller-provided I/O
 * sink and a bounded @c ove_arena program break. Unknown syscalls return
 * @c -OVE_LNX_ENOSYS.
 *
 * @note Requires @c CONFIG_OVE_LINUX.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "ove/arena.h"
#include "ove/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux ARM EABI syscall numbers (subset). */
#define OVE_LNX_NR_exit 1
#define OVE_LNX_NR_read 3
#define OVE_LNX_NR_write 4
#define OVE_LNX_NR_brk 45
#define OVE_LNX_NR_writev 146
#define OVE_LNX_NR_exit_group 248

/* Linux errno values returned (negated) on syscall failure. */
#define OVE_LNX_EBADF 9
#define OVE_LNX_EFAULT 14
#define OVE_LNX_EINVAL 22
#define OVE_LNX_ENOSYS 38

/** Scatter/gather element, matching the target's @c struct iovec layout. */
typedef struct ove_lnx_iovec {
	void *iov_base; /**< Start of the buffer (in the program's address space). */
	size_t iov_len; /**< Length of the buffer in bytes. */
} ove_lnx_iovec;

/** fd 1/2 output sink. Returns bytes written or a negated Linux errno. */
typedef long (*ove_lnx_write_fn)(void *ctx, int fd, const void *buf, size_t len);
/** fd 0 input source. Returns bytes read (0 = EOF) or a negated Linux errno. */
typedef long (*ove_lnx_read_fn)(void *ctx, int fd, void *buf, size_t len);

/**
 * @brief A Linux process context — the state syscalls act on.
 *
 * Minimal Phase-A model: a bounded program break carved from an @c ove_arena,
 * standard-stream I/O via caller-set callbacks, and an exit latch. A full fd
 * table / signal state / mmap land in later phases.
 */
typedef struct ove_lnx_proc {
	ove_arena_t *arena;	   /**< Backs @c brk (and, later, mmap(ANON)). */
	uintptr_t brk_base;	   /**< Initial program break. */
	uintptr_t brk_cur;	   /**< Current program break. */
	uintptr_t brk_max;	   /**< Ceiling imposed by the arena reservation. */
	ove_lnx_write_fn write_fn; /**< fd 1/2 sink; NULL → @c -OVE_LNX_EBADF. */
	ove_lnx_read_fn read_fn;   /**< fd 0 source; NULL → EOF. */
	void *io_ctx;		   /**< Opaque, passed to @c write_fn / @c read_fn. */
	int exited;		   /**< Set once @c exit / @c exit_group is called. */
	int exit_status;	   /**< Low 8 bits of the exit code. */
} ove_lnx_proc_t;

/**
 * @brief Initialise a process context with an arena-backed program break.
 *
 * Reserves @p brk_bytes from @p arena for the program break. The caller wires
 * @c write_fn / @c read_fn / @c io_ctx afterwards.
 *
 * @return OVE_OK; OVE_ERR_INVALID_PARAM on bad arguments;
 *         OVE_ERR_NO_MEMORY if the arena cannot satisfy @p brk_bytes.
 * @note Requires @c CONFIG_OVE_LINUX.
 */
int ove_lnx_proc_init(ove_lnx_proc_t *proc, ove_arena_t *arena, size_t brk_bytes);

/**
 * @brief Dispatch one Linux syscall against @p proc.
 *
 * @param[in] proc Process context.
 * @param[in] nr   Linux syscall number (@c OVE_LNX_NR_*).
 * @param[in] a0..a5 Syscall arguments (register values; pointers are program
 *                   addresses).
 * @return The syscall result, Linux-ABI style: a non-negative value on success
 *         or a negated errno on failure. Unknown numbers return
 *         @c -OVE_LNX_ENOSYS.
 * @note Requires @c CONFIG_OVE_LINUX.
 */
long ove_lnx_syscall(ove_lnx_proc_t *proc, long nr, long a0, long a1, long a2, long a3, long a4,
		     long a5);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LINUX_SYSCALL_H */
