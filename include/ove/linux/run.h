/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_RUN_H
#define OVE_LINUX_RUN_H

#include "ove/linux/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @defgroup ove_lnx_run Linux personality runner
 * @brief Engine-agnostic public API for running a Linux program under the
 *        oveRTOS Linux personality.
 *
 * The engine-agnostic core (@ref ove_lnx_syscall) translates the Linux ABI into
 * oveRTOS primitives; a per-engine SEAM binds it to a concrete RTOS engine —
 * trapping the unprivileged program's syscalls, running each loaded bFLT in its
 * own isolated memory domain, and implementing the NOMMU process model
 * (sequentialised vfork/exec/wait, signal delivery, the run loop). This header
 * is the public contract a host application uses; the seam provides the
 * implementation (currently @c backends/zephyr/zephyr_lnx.c for Cortex-M with
 * @c CONFIG_USERSPACE).
 *
 * A host supplies a parsed rootfs and console callbacks, then calls
 * @ref ove_lnx_run with an init program.
 * @{
 */

/** Host configuration for a personality run. */
typedef struct {
	const ove_lnx_file_t *rootfs; /**< Parsed (read-only) rootfs table. */
	int rootfs_count;	      /**< Entry count in @p rootfs. */
	ove_lnx_write_fn write_fn;    /**< Console sink (fd 1/2). */
	ove_lnx_read_fn read_fn;      /**< Console source (fd 0); see the tty helpers. */
	void *io_ctx;		      /**< Opaque, passed to @p write_fn / @p read_fn. */
	void (*on_enosys)(long nr);   /**< Optional: notified of an unimplemented syscall. */
} ove_lnx_run_config_t;

/** @ref ove_lnx_run outcomes (negative; a non-negative result is the init
 * process's exit status). */
#define OVE_LNX_RUN_ELAUNCH (-1)  /**< The init program could not be loaded. */
#define OVE_LNX_RUN_EEXEC (-2)	  /**< A child execve relaunch failed. */
#define OVE_LNX_RUN_ETIMEOUT (-3) /**< init did not exit within the run budget. */

/**
 * Load @p path from the rootfs and run it as pid 1, driving the NOMMU process
 * model (vfork/exec/wait, signals, pipes) until it exits.
 *
 * @p argv[0] is the program name seen by the program (it may differ from
 * @p path, e.g. run @c /bin/busybox as @c "sh"). @p path must name a regular
 * file in @p cfg->rootfs. Calls are sequential: each run tears down its threads
 * before returning, so a host may call this repeatedly.
 *
 * @return the init exit status (>= 0), or one of the @c OVE_LNX_RUN_E* codes (< 0).
 */
int ove_lnx_run(const ove_lnx_run_config_t *cfg, const char *path, int argc,
		const char *const argv[]);

/**
 * Whether the tty is in ISIG (canonical) mode. A @c read_fn consults this to
 * decide whether a console ^C is the interrupt key (raise SIGINT) or a literal
 * byte (the shell's raw line editor turns ISIG off). Tracked from TCSETS.
 */
int ove_lnx_tty_isig(void);

/**
 * Latch an asynchronous signal (e.g. SIGINT from a console ^C) for delivery to
 * the running program at the next syscall boundary (the Linux async-delivery
 * model). Typically called by a @c read_fn that is returning @c -OVE_LNX_EINTR.
 */
void ove_lnx_post_signal(int sig);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_RUN_H */
