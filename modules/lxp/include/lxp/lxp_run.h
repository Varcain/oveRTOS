/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_RUN_H
#define OVE_LINUX_RUN_H

#include "lxp/lxp_syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * @defgroup lxp_run Linux personality runner
 * @brief Engine-agnostic public API for running a Linux program under the
 *        oveRTOS Linux personality.
 *
 * The engine-agnostic core (@ref lxp_syscall) translates the Linux ABI into
 * oveRTOS primitives; a per-engine SEAM binds it to a concrete RTOS engine —
 * trapping the unprivileged program's syscalls, running each loaded FDPIC program in its
 * own isolated memory domain, and implementing the NOMMU process model
 * (sequentialised vfork/exec/wait, signal delivery, the run loop). This header
 * is the public contract a host application uses; the seam provides the
 * implementation (currently @c backends/zephyr/zephyr_lnx.c for Cortex-M with
 * @c CONFIG_USERSPACE).
 *
 * A host supplies a parsed rootfs and console callbacks, then calls
 * @ref lxp_run with an init program.
 * @{
 */

/** Host configuration for a personality run. */
typedef struct {
	const lxp_file_t *rootfs; /**< Parsed (read-only) rootfs table. */
	int rootfs_count;	      /**< Entry count in @p rootfs. */
	lxp_write_fn write_fn;    /**< Console sink (fd 1/2). */
	lxp_read_fn read_fn;      /**< Console source (fd 0); see the tty helpers. */
	void *io_ctx;		      /**< Opaque, passed to @p write_fn / @p read_fn. */
	void (*on_enosys)(long nr);   /**< Optional: notified of an unimplemented syscall. */
	/** Optional: non-blocking "is a console keystroke available right now?" (1/0).
	 * Enables a true poll(2) on the console fd (e.g. interactive `top`'s 'q' quit):
	 * without it the console transport is blocking-only and poll falls back to a
	 * heuristic. Backed by a UART RX-ready check when the host uses a UART console. */
	int (*console_poll)(void *ctx);
} lxp_run_config_t;

/** @ref lxp_run outcomes (negative; a non-negative result is the init
 * process's exit status). */
#define LXP_RUN_ELAUNCH (-1)  /**< The init program could not be loaded. */
#define LXP_RUN_EEXEC (-2)	  /**< A child execve relaunch failed. */
#define LXP_RUN_ETIMEOUT (-3) /**< init did not exit within the run budget. */

/**
 * Load @p path from the rootfs and run it as pid 1, driving the NOMMU process
 * model (vfork/exec/wait, signals, pipes) until it exits.
 *
 * @p argv[0] is the program name seen by the program (it may differ from
 * @p path, e.g. run @c /bin/busybox as @c "sh"). @p path must name a regular
 * file in @p cfg->rootfs. Calls are sequential: each run tears down its threads
 * before returning, so a host may call this repeatedly.
 *
 * @return the init exit status (>= 0), or one of the @c LXP_RUN_E* codes (< 0).
 */
int lxp_run(const lxp_run_config_t *cfg, const char *path, int argc,
		const char *const argv[]);

/**
 * Whether the tty is in ISIG (canonical) mode. A @c read_fn consults this to
 * decide whether a console ^C is the interrupt key (raise SIGINT) or a literal
 * byte (the shell's raw line editor turns ISIG off). Tracked from TCSETS.
 */
int lxp_tty_isig(void);

/**
 * Latch an asynchronous signal (e.g. SIGINT from a console ^C) for delivery to
 * the running program at the next syscall boundary (the Linux async-delivery
 * model). Typically called by a @c read_fn that is returning @c -LXP_EINTR.
 */
void lxp_post_signal(int sig);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_RUN_H */
