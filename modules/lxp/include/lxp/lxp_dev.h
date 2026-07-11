/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_LINUX_DEV_H
#define OVE_LINUX_DEV_H

/**
 * @file dev.h
 * @defgroup ove_linux_dev Linux personality device layer
 * @ingroup ove_linux
 * @brief Character-device nodes under /dev for the Linux personality.
 *
 * A small in-kernel device model bolted onto the syscall layer: class drivers
 * register an @ref lxp_dev (a path like "/dev/fb0" + an ops vtable) and the
 * personality routes open/read/write/ioctl/poll/lseek/mmap on that path to the
 * driver. Engine-agnosticism is free — drivers bridge to the @c ove_* public
 * HALs (ove_fb, ove_i2c, ...), never to per-engine code.
 *
 * Blocking model: like the syscall layer, driver entry points run in the
 * SVC/exception context, so they must NOT block inline. A driver that would
 * block returns @c -LXP_EAGAIN; the core parks the calling process
 * (@c dev_wait) and the run-loop coordinator retries the op on its own thread
 * — the same park/retry pattern the pipe layer uses (see lxp_pipe_retry).
 *
 * @note Requires @c LXP_ENABLE_DEV.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_port.h"
#include "lxp/lxp_syscall.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lxp_dev;
struct lxp_dev_open;

/**
 * @brief Per-class driver operations.
 *
 * All entry points run in the SVC/exception context (except @c tick, run on the
 * coordinator thread) and return a Linux-ABI value: @c >=0 on success or a
 * negated @c LXP_E* on failure. A blocking read/write/ioctl returns
 * @c -LXP_EAGAIN to have the core park + retry the caller. Any user pointer
 * a handler dereferences must first pass @c user_ok (handlers run PRIVILEGED).
 */
struct lxp_dev_ops {
	/** Open: initialise @p o for this open (optional). */
	long (*open)(struct lxp_dev *d, struct lxp_dev_open *o, int flags);
	/** Release: last close of this open (optional). */
	long (*release)(struct lxp_dev *d, struct lxp_dev_open *o);
	/** read(2): bytes read, 0 (EOF), or -EAGAIN to block. @p buf is pre-@c user_ok'd. */
	long (*read)(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p, void *buf,
		     size_t len);
	/** write(2): bytes written, or -EAGAIN to block. @p buf is pre-@c user_ok'd. */
	long (*write)(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		      const void *buf, size_t len);
	/** ioctl(2): every user deref of @p arg must pass @c user_ok. -ENOTTY = unknown cmd. */
	long (*ioctl)(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p,
		      unsigned long cmd, unsigned long arg);
	/** poll(2): POLLIN|POLLOUT bits currently ready; must never block. */
	unsigned (*poll)(struct lxp_dev *d, struct lxp_dev_open *o);
	/** mmap(2): fill @p phys with the device buffer address + @p attrs; the core
	 *  defers the MPU work to the engine seam. NULL => -ENODEV. (Phase P3.) */
	long (*mmap)(struct lxp_dev *d, struct lxp_dev_open *o, lxp_proc_t *p, size_t len,
		     uint32_t pgoff, uintptr_t *phys, unsigned *attrs);
};

/** A registered character device. */
struct lxp_dev {
	const char *path;		     /**< Absolute node path, e.g. "/dev/fb0". */
	const struct lxp_dev_ops *ops;   /**< Class operations. */
	void *drv;			     /**< Class-driver instance state. */
	uint16_t major, minor;		     /**< Real Linux device numbers (for st_rdev). */
	uint32_t size;			     /**< Seekable extent (0 => lseek -ESPIPE). */
};

/**
 * @brief Per-open state, pooled (the fd slot is too small for driver cursors).
 *
 * A device fd's @c file_idx indexes @c g_lnx_devopen[]. fork/dup share an open
 * (refcounted); the last close calls @c ops->release.
 */
struct lxp_dev_open {
	uint8_t used;	 /**< Slot allocated. */
	uint8_t refs;	 /**< Shares across fork/dup; release at 0. */
	uint8_t dev;	 /**< Registered-device index. */
	uint16_t oflags; /**< open(2) flags (O_NONBLOCK gates blocking). */
	uint32_t pos;	 /**< Seek cursor (fb byte offset, ...). */
	/* Class-private per-open cursors; extended as device classes land. */
	union {
		struct {
			uint16_t addr;
		} i2c; /**< I2C_SLAVE address (P2 i2c). */
		struct {
			uint16_t tail;
			uint8_t overrun;
		} input; /**< evdev ring cursor (P4 input). */
	} u;
};

/* LXP_MAP_NC/WT/DEV (map_device attribute hints) come from lxp_port.h. */

/** dev_wait op codes: which parked device op the coordinator retries/completes.
 *  Shared with the run loop (backends/common/lxp_run.c) so it can special-case
 *  DEVW_MMAP (the only one needing the engine's map_device seam). */
#define LXP_DEVW_READ 1u
#define LXP_DEVW_WRITE 2u
#define LXP_DEVW_IOCTL 3u
#define LXP_DEVW_MMAP 4u /**< P3: coordinator installs the MPU region, resumes r0 = mapped addr. */

/**
 * @brief Register a character device (class driver or board/app custom).
 *
 * Call before @c lxp_run (or from @c lxp_dev_autoreg_all). @p dev is
 * copied into the registry; its @c ops / @c drv must have static lifetime.
 * @return 0, or a negative errno (@c -EMFILE if the registry is full).
 */
int lxp_dev_register(const struct lxp_dev *dev);

/**
 * @brief Wake the run-loop coordinator to retry parked device I/O promptly.
 *
 * Called from a driver's data-ready path (uart rx, input feeder, audio period)
 * so a parked reader resumes at once instead of at the next poll interval.
 * Defined by the run loop (posts its coordinator event); weak no-op otherwise.
 */
void lxp_dev_kick(void);

/**
 * @brief Engine-neutral touch-input feeder (Phase P4).
 *
 * A touch driver (FT5336) or a test injector calls this from the coordinator
 * tick; the evdev class turns it into /dev/input/event0 events. Weak no-op
 * until the input class is registered.
 */
void lxp_input_report_touch(int x, int y, int pressed);

/* ---- syscall-layer <-> core interface (called from ove_linux_syscall.c) ---- */
/* These are the seams the FD_DEV branches of the syscall handlers call. They
 * are compiled only when LXP_ENABLE_DEV is set (the branches are #if'd),
 * so no weak fallbacks are needed — the core is always linked when the feature
 * is on (firmware) or under test (host cmocka). */

/** Registered-device index for @p abspath, or -1 (not a device node). */
int lxp_dev_lookup(const char *abspath);
/** Open device @p devidx: allocate an open slot + run @c ops->open. Returns the
 *  open-pool index (the fd's file_idx) or a negative errno. */
long lxp_dev_open_new(lxp_proc_t *p, int devidx, int flags);
/** Drop a reference on open @p oi (close/exit); @c ops->release at the last. */
void lxp_dev_close(int oi);
/** Add a reference on open @p oi (dup/fork inheritance). */
void lxp_dev_get(int oi);
/** fcntl F_SETFL / F_GETFL: the open's status flags (O_NONBLOCK gates blocking;
 *  LVGL's evdev sets O_NONBLOCK via fcntl after open). */
void lxp_dev_setfl(int oi, int flags);
int lxp_dev_getfl(int oi);
long lxp_dev_read(lxp_proc_t *p, int oi, void *buf, size_t len);
long lxp_dev_write(lxp_proc_t *p, int oi, const void *buf, size_t len);
/** Positioned read/write at @p off without moving the fd cursor (pread/pwrite;
 *  LVGL's fbdev writes scanlines this way). Does not park (fb is inline). */
long lxp_dev_pread(lxp_proc_t *p, int oi, void *buf, size_t len, uint32_t off);
long lxp_dev_pwrite(lxp_proc_t *p, int oi, const void *buf, size_t len, uint32_t off);
long lxp_dev_ioctl(lxp_proc_t *p, int oi, unsigned long cmd, unsigned long arg);
/** mmap(2) a device buffer (P3): resolve the physical range via the driver's .mmap
 *  op, then park (DEVW_MMAP) so the coordinator installs the MPU region + resumes
 *  the proc with r0 = the mapped address. Returns 0 (parked) or a negative errno. */
long lxp_dev_mmap(lxp_proc_t *p, int oi, size_t len, uint32_t pgoff);
unsigned lxp_dev_poll(int oi);
/** lseek on open @p oi within the device's @c size; -ESPIPE if not seekable. */
long lxp_dev_lseek(int oi, long off, int whence);
/** Fill S_IFCHR mode / st_rdev / size for fstat/statx of open @p oi. */
void lxp_dev_fstat(int oi, uint32_t *mode, uint64_t *rdev, uint64_t *size);
/** Path-based stat: fill mode/rdev for a device @p abspath; -1 if not a device. */
int lxp_dev_stat_path(const char *abspath, uint32_t *mode, uint64_t *rdev);
/** Registered-device count + i-th path/mode/rdev, for the /dev getdents listing. */
int lxp_dev_count(void);
const char *lxp_dev_path(int i, uint32_t *mode);

/** Register a coordinator-thread tick callback (fb present @ ~30 Hz, FT5336 poll
 *  @ ~60 Hz); a class driver calls this from its autoreg. */
void lxp_dev_tick_register(void (*fn)(uint64_t now_us));

/** access_ok for a driver's ioctl handler to validate its user pointer (the
 *  confused-deputy guard — device handlers run PRIVILEGED). read/write buffers
 *  are already validated by the core; ioctl args are not. Defined in
 *  ove_linux_syscall.c. */
int user_ok(const lxp_proc_t *p, const void *ptr, size_t len, int write);

/** Retry a parked device op for the coordinator; result or -EAGAIN (still blocked). */
long lxp_dev_retry(lxp_proc_t *p);
/** Coordinator-thread periodic work (fb flush, touch poll). @p now_us = ove_time_get_us. */
void lxp_dev_tick(uint64_t now_us);
/** Register the Kconfig-enabled class drivers (run once on the coordinator thread). */
void lxp_dev_autoreg_all(void);
/** fork: the child inherited the parent's FD_DEV fds — add a reference to each. */
void lxp_dev_fork_inherit(lxp_proc_t *child);
/** exit: release every FD_DEV open the process still holds. */
void lxp_dev_proc_exit(lxp_proc_t *p);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LINUX_DEV_H */
