/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Linux-personality character-device core: a registry of /dev nodes + a pooled
 * per-open state table, and the routing the FD_DEV branches of the syscall
 * handlers call into. Class drivers (fb, input, i2c, ...) register an
 * lxp_dev and bridge to the engine-neutral ove_* HALs.
 *
 * Blocking is deferred, never inline: a driver op that would block returns
 * -EAGAIN; this core parks the caller (proc->dev_wait) and the run-loop
 * coordinator retries via lxp_dev_retry — mirroring the pipe park/retry.
 */

#include "lxp/lxp_config.h"

#if defined(LXP_ENABLE_DEV)

#include "lxp/lxp_dev.h"

#include <string.h>

/* fd-slot kind for a device fd (fds[].file_idx = open-pool index). Kept in step
 * with the FD_* enumeration in ove_linux_syscall.c (free/console/file/pipe/
 * tmpfs/proc = 0..5). */
#ifndef LXP_FD_DEV
#define LXP_FD_DEV 6
#endif

/* O_NONBLOCK (ARM): a non-blocking open returns -EAGAIN instead of parking. */
#ifndef LXP_O_NONBLOCK
#define LXP_O_NONBLOCK 0x800
#endif

/* proc->dev_wait states (LXP_DEVW_*) now live in ove/linux/dev.h, shared with
 * the run loop so it can special-case DEVW_MMAP (needs the engine map_device seam). */

#define LXP_NDEV 16	    /* max registered device nodes */
#define LXP_NDEVOPEN 16 /* max concurrent device opens (pooled) */
#define LXP_NDEVTICK 4  /* max coordinator-tick callbacks (fb flush, touch poll) */

static struct lxp_dev g_lnx_devs[LXP_NDEV];
static int g_lnx_ndev;
static struct lxp_dev_open g_lnx_devopen[LXP_NDEVOPEN];
static void (*g_lnx_devtick[LXP_NDEVTICK])(uint64_t now_us);
static int g_lnx_ndevtick;

/* ---- registration ---------------------------------------------------------- */
int lxp_dev_register(const struct lxp_dev *dev)
{
	if (!dev || !dev->path || !dev->ops)
		return -LXP_EINVAL;
	if (g_lnx_ndev >= LXP_NDEV)
		return -LXP_EMFILE;
	/* A re-register of the same path replaces the entry (idempotent autoreg). */
	for (int i = 0; i < g_lnx_ndev; i++)
		if (strcmp(g_lnx_devs[i].path, dev->path) == 0) {
			g_lnx_devs[i] = *dev;
			return 0;
		}
	g_lnx_devs[g_lnx_ndev++] = *dev;
	return 0;
}

/* Register a coordinator-tick callback (fb flush @ ~30 Hz, FT5336 poll @ ~60 Hz).
 * Called from a class driver's autoreg. Non-public helper (declared in the class
 * drivers via this prototype). */
void lxp_dev_tick_register(void (*fn)(uint64_t now_us));
void lxp_dev_tick_register(void (*fn)(uint64_t now_us))
{
	if (fn && g_lnx_ndevtick < LXP_NDEVTICK)
		g_lnx_devtick[g_lnx_ndevtick++] = fn;
}

int lxp_dev_lookup(const char *abspath)
{
	for (int i = 0; i < g_lnx_ndev; i++)
		if (strcmp(g_lnx_devs[i].path, abspath) == 0)
			return i;
	return -1;
}

int lxp_dev_count(void)
{
	return g_lnx_ndev;
}

const char *lxp_dev_path(int i, uint32_t *mode)
{
	if (i < 0 || i >= g_lnx_ndev)
		return NULL;
	if (mode)
		*mode = LXP_S_IFCHR | 0666u;
	return g_lnx_devs[i].path;
}

/* ---- open pool ------------------------------------------------------------- */
static struct lxp_dev_open *open_slot(int oi)
{
	if (oi < 0 || oi >= LXP_NDEVOPEN || !g_lnx_devopen[oi].used)
		return NULL;
	return &g_lnx_devopen[oi];
}

long lxp_dev_open_new(lxp_proc_t *p, int devidx, int flags)
{
	(void)p;
	if (devidx < 0 || devidx >= g_lnx_ndev)
		return -LXP_ENOENT;
	int oi = -1;
	for (int i = 0; i < LXP_NDEVOPEN; i++)
		if (!g_lnx_devopen[i].used) {
			oi = i;
			break;
		}
	if (oi < 0)
		return -LXP_EMFILE;
	struct lxp_dev_open *o = &g_lnx_devopen[oi];
	memset(o, 0, sizeof(*o));
	o->used = 1;
	o->refs = 1;
	o->dev = (uint8_t)devidx;
	o->oflags = (uint16_t)flags;
	struct lxp_dev *d = &g_lnx_devs[devidx];
	if (d->ops->open) {
		long r = d->ops->open(d, o, flags);
		if (r < 0) {
			o->used = 0;
			return r;
		}
	}
	return oi;
}

void lxp_dev_get(int oi)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (o && o->refs < 0xff)
		o->refs++;
}

void lxp_dev_setfl(int oi, int flags)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (o)
		o->oflags = (uint16_t)flags;
}

int lxp_dev_getfl(int oi)
{
	struct lxp_dev_open *o = open_slot(oi);
	return o ? o->oflags : 0;
}

void lxp_dev_close(int oi)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return;
	if (o->refs > 1) {
		o->refs--;
		return;
	}
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (d->ops->release)
		d->ops->release(d, o);
	o->used = 0;
}

/* ---- read / write / ioctl (with deferred-block park) ----------------------- */
long lxp_dev_read(lxp_proc_t *p, int oi, void *buf, size_t len)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->read)
		return -LXP_EINVAL;
	long r = d->ops->read(d, o, p, buf, len);
	if (r == -LXP_EAGAIN) {
		if (o->oflags & LXP_O_NONBLOCK)
			return -LXP_EAGAIN;
		p->dev_wait = LXP_DEVW_READ; /* park: the coordinator retries */
		p->dev_oi = oi;
		p->dev_buf = (uintptr_t)buf;
		p->dev_len = len;
		return 0;
	}
	return r;
}

long lxp_dev_write(lxp_proc_t *p, int oi, const void *buf, size_t len)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->write)
		return -LXP_EINVAL;
	long r = d->ops->write(d, o, p, buf, len);
	if (r == -LXP_EAGAIN) {
		if (o->oflags & LXP_O_NONBLOCK)
			return -LXP_EAGAIN;
		p->dev_wait = LXP_DEVW_WRITE;
		p->dev_oi = oi;
		p->dev_buf = (uintptr_t)buf;
		p->dev_len = len;
		return 0;
	}
	return r;
}

long lxp_dev_ioctl(lxp_proc_t *p, int oi, unsigned long cmd, unsigned long arg)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->ioctl)
		return -LXP_ENOTTY;
	long r = d->ops->ioctl(d, o, p, cmd, arg);
	if (r == -LXP_EAGAIN) {
		if (o->oflags & LXP_O_NONBLOCK)
			return -LXP_EAGAIN;
		p->dev_wait = LXP_DEVW_IOCTL;
		p->dev_oi = oi;
		p->dev_cmd = cmd;
		p->dev_buf = arg;
		return 0;
	}
	return r;
}

/* mmap(2) a device buffer (P3): the driver's .mmap op resolves the physical range +
 * cache attrs (e.g. /dev/fb0 -> the LTDC framebuffer, Normal-NC), then we PARK on
 * DEVW_MMAP. Adding the unprivileged MPU region over that range is a domain/TCB edit
 * that is not safe from the svc-exception dispatch, so the run-loop coordinator does
 * it (eng->map_device) and resumes the proc with r0 = the mapped address. */
long lxp_dev_mmap(lxp_proc_t *p, int oi, size_t len, uint32_t pgoff)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->mmap)
		return -LXP_ENODEV; /* not mappable -> caller falls back to the write path */
	uintptr_t phys = 0;
	unsigned attrs = LXP_MAP_NC;
	long r = d->ops->mmap(d, o, p, len, pgoff, &phys, &attrs);
	if (r < 0)
		return r;
	p->dev_wait = LXP_DEVW_MMAP;
	p->dev_oi = oi;
	p->dev_buf = phys;  /* physical base to map */
	p->dev_len = len;   /* extent */
	p->dev_cmd = attrs; /* LXP_MAP_* hint for the engine seam */
	return 0;
}

/* Positioned I/O: drive the same read/write op at `off` with the fd cursor
 * preserved (pread/pwrite semantics). The fb is inline (never -EAGAIN), so this
 * does not park; a blocking device would need the dev_wait path instead. */
long lxp_dev_pread(lxp_proc_t *p, int oi, void *buf, size_t len, uint32_t off)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->read)
		return -LXP_EINVAL;
	uint32_t save = o->pos;
	o->pos = off;
	long r = d->ops->read(d, o, p, buf, len);
	o->pos = save;
	return r;
}

long lxp_dev_pwrite(lxp_proc_t *p, int oi, const void *buf, size_t len, uint32_t off)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->write)
		return -LXP_EINVAL;
	uint32_t save = o->pos;
	o->pos = off;
	long r = d->ops->write(d, o, p, buf, len);
	o->pos = save;
	return r;
}

unsigned lxp_dev_poll(int oi)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return 0;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	return d->ops->poll ? d->ops->poll(d, o) : (LXP_POLLIN | LXP_POLLOUT);
}

long lxp_dev_lseek(int oi, long off, int whence)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (d->size == 0)
		return -LXP_ESPIPE; /* a non-seekable device (no fixed extent) */
	long base;
	switch (whence) {
	case LXP_SEEK_SET:
		base = 0;
		break;
	case LXP_SEEK_CUR:
		base = (long)o->pos;
		break;
	case LXP_SEEK_END:
		base = (long)d->size;
		break;
	default:
		return -LXP_EINVAL;
	}
	long pos = base + off;
	if (pos < 0 || pos > (long)d->size)
		return -LXP_EINVAL;
	o->pos = (uint32_t)pos;
	return pos;
}

/* ---- stat / getdents helpers ----------------------------------------------- */
void lxp_dev_fstat(int oi, uint32_t *mode, uint64_t *rdev, uint64_t *size)
{
	struct lxp_dev_open *o = open_slot(oi);
	if (!o) {
		if (mode)
			*mode = LXP_S_IFCHR | 0666u;
		if (rdev)
			*rdev = 0;
		if (size)
			*size = 0;
		return;
	}
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	if (mode)
		*mode = LXP_S_IFCHR | 0666u;
	if (rdev)
		*rdev = ((uint64_t)d->major << 8) | d->minor;
	if (size)
		*size = d->size;
}

int lxp_dev_stat_path(const char *abspath, uint32_t *mode, uint64_t *rdev)
{
	int di = lxp_dev_lookup(abspath);
	if (di < 0)
		return -1;
	struct lxp_dev *d = &g_lnx_devs[di];
	if (mode)
		*mode = LXP_S_IFCHR | 0666u;
	if (rdev)
		*rdev = ((uint64_t)d->major << 8) | d->minor;
	return 0;
}

/* ---- coordinator: retry parked device I/O + periodic tick ------------------ */
long lxp_dev_retry(lxp_proc_t *p)
{
	int oi = p->dev_oi;
	struct lxp_dev_open *o = open_slot(oi);
	if (!o)
		return -LXP_EBADF;
	struct lxp_dev *d = &g_lnx_devs[o->dev];
	switch (p->dev_wait) {
	case LXP_DEVW_READ:
		return d->ops->read ? d->ops->read(d, o, p, (void *)p->dev_buf, p->dev_len)
				    : -LXP_EINVAL;
	case LXP_DEVW_WRITE:
		return d->ops->write ? d->ops->write(d, o, p, (const void *)p->dev_buf, p->dev_len)
				     : -LXP_EINVAL;
	case LXP_DEVW_IOCTL:
		return d->ops->ioctl ? d->ops->ioctl(d, o, p, p->dev_cmd, p->dev_buf)
				     : -LXP_ENOTTY;
	default:
		return -LXP_EINVAL;
	}
}

void lxp_dev_tick(uint64_t now_us)
{
	for (int i = 0; i < g_lnx_ndevtick; i++)
		g_lnx_devtick[i](now_us);
}

/* ---- fork / exit fd lifecycle ---------------------------------------------- */
void lxp_dev_fork_inherit(lxp_proc_t *child)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (child->fds[fd].kind == LXP_FD_DEV)
			lxp_dev_get(child->fds[fd].file_idx);
}

void lxp_dev_proc_exit(lxp_proc_t *p)
{
	for (int fd = 0; fd < LXP_MAX_FDS; fd++)
		if (p->fds[fd].kind == LXP_FD_DEV) {
			lxp_dev_close(p->fds[fd].file_idx);
			p->fds[fd].kind = 0; /* FD_FREE (private to the syscall layer) */
		}
}

/* ---- Kconfig-auto class registration --------------------------------------- */
/* Each class driver (fb, input, ...) provides lxp_dev_autoreg_<c>() behind its
 * LXP_ENABLE_DEV_<C>. Gate the CALLS on the same config rather than relying on
 * weak no-op fallbacks: a weak fallback here would be bound to the same-TU definition
 * by GCC's default -fno-semantic-interposition, and the class object — reachable only
 * through this hook — would never be pulled from the archive (so /dev/fb0 /
 * /dev/input/event0 would silently not register on an archive+GC link, e.g. NuttX).
 * Gating makes the call a direct reference to the compiled class's strong definition.
 * Run once on the coordinator thread (blocking HAL init — ove_fb_init / ove_i2c_create
 * — is legal there). */
#if defined(LXP_ENABLE_DEV_FB)
void lxp_dev_autoreg_fb(void);
#endif
#if defined(LXP_ENABLE_DEV_INPUT)
void lxp_dev_autoreg_input(void);
#endif

void lxp_dev_autoreg_all(void)
{
#if defined(LXP_ENABLE_DEV_FB)
	lxp_dev_autoreg_fb();
#endif
#if defined(LXP_ENABLE_DEV_INPUT)
	lxp_dev_autoreg_input();
#endif
}

/* Weak input feeder so the core links before the evdev class (P4) defines it. */
__attribute__((weak)) void lxp_input_report_touch(int x, int y, int pressed)
{
	(void)x;
	(void)y;
	(void)pressed;
}

/* Weak coordinator kick: the run loop provides the strong version (posts its
 * event). The host cmocka test links the core without the run loop, so this
 * no-op keeps a driver's lxp_dev_kick() call resolvable there. */
__attribute__((weak)) void lxp_dev_kick(void) {}

#endif /* LXP_ENABLE_DEV */
