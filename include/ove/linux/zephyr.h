/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_ZEPHYR_H
#define OVE_LINUX_ZEPHYR_H

#include "ove/linux/syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file
 * Zephyr engine seam for the Linux personality.
 *
 * The engine-agnostic core (@ref ove_lnx_syscall) translates the Linux ABI into
 * oveRTOS primitives; this seam binds it to a Zephyr/Cortex-M target. It traps
 * the unprivileged program's @c svc (via a linker @c --wrap of
 * @c z_do_kernel_oops — no Zephyr source patch), runs each loaded bFLT in its
 * own MPU memory domain, and implements the NOMMU process model
 * (sequentialised vfork/exec/wait, signal delivery, and the run loop).
 *
 * A host supplies a parsed rootfs and console callbacks, then calls
 * @ref ove_lnx_zephyr_run with an init program; the seam owns the program
 * memory regions, the slot/domain bookkeeping, and the orchestration.
 *
 * Requires @c CONFIG_USERSPACE and the link option
 * @c -Wl,--wrap=z_do_kernel_oops.
 */

/** Host configuration for a personality run. */
typedef struct {
	const ove_lnx_file_t *rootfs; /**< Parsed (read-only) rootfs table. */
	int rootfs_count;	      /**< Entry count in @p rootfs. */
	ove_lnx_write_fn write_fn;    /**< Console sink (fd 1/2). */
	ove_lnx_read_fn read_fn;      /**< Console source (fd 0); see the tty helpers. */
	void *io_ctx;		      /**< Opaque, passed to @p write_fn / @p read_fn. */
	void (*on_enosys)(long nr);   /**< Optional: notified of an unimplemented syscall. */
} ove_lnx_zephyr_config_t;

/** @ref ove_lnx_zephyr_run outcomes (negative; a non-negative result is the
 * init process's exit status). */
#define OVE_LNX_ZEPHYR_ELAUNCH (-1)  /**< The init program could not be loaded. */
#define OVE_LNX_ZEPHYR_EEXEC (-2)    /**< A child execve relaunch failed. */
#define OVE_LNX_ZEPHYR_ETIMEOUT (-3) /**< init did not exit within the run budget. */

/**
 * Load @p path from the rootfs and run it as pid 1, driving the NOMMU process
 * model (vfork/exec/wait, signals, pipes) until it exits.
 *
 * @p argv[0] is the program name seen by the program (it may differ from
 * @p path, e.g. run @c /bin/busybox as @c "sh"). @p path must name a regular
 * file in @p cfg->rootfs.
 *
 * @return the init exit status (>= 0), or one of the @c OVE_LNX_ZEPHYR_E*
 *         codes (< 0).
 */
int ove_lnx_zephyr_run(const ove_lnx_zephyr_config_t *cfg, const char *path, int argc,
		       const char *const argv[]);

/**
 * Whether the tty is in ISIG (canonical) mode. A @c read_fn consults this to
 * decide whether a console ^C is the interrupt key (raise SIGINT) or a literal
 * byte (the shell's raw line editor turns ISIG off). Tracked from TCSETS.
 */
int ove_lnx_zephyr_tty_isig(void);

/**
 * Latch an asynchronous signal (e.g. SIGINT from a console ^C) for delivery to
 * the running program at the next syscall boundary (the Linux async-delivery
 * model). Typically called by a @c read_fn that is returning @c -OVE_LNX_EINTR.
 */
void ove_lnx_zephyr_post_signal(int sig);

#ifdef __cplusplus
}
#endif

#endif /* OVE_LINUX_ZEPHYR_H */
