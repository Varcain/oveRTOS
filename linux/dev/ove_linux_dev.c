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
 * ove_lnx_dev and bridge to the engine-neutral ove_* HALs.
 *
 * Blocking is deferred, never inline: a driver op that would block returns
 * -EAGAIN; this core parks the caller (proc->dev_wait) and the run-loop
 * coordinator retries via ove_lnx_dev_retry — mirroring the pipe park/retry.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX_DEV)

#include "ove/linux/dev.h"

#include <string.h>

/* fd-slot kind for a device fd (fds[].file_idx = open-pool index). Kept in step
 * with the FD_* enumeration in ove_linux_syscall.c (free/console/file/pipe/
 * tmpfs/proc = 0..5). */
#ifndef OVE_LNX_FD_DEV
#define OVE_LNX_FD_DEV 6
#endif

/* O_NONBLOCK (ARM): a non-blocking open returns -EAGAIN instead of parking. */
#ifndef OVE_LNX_O_NONBLOCK
#define OVE_LNX_O_NONBLOCK 0x800
#endif

/* proc->dev_wait states (what op the coordinator must retry). */
#define OVE_LNX_DEVW_READ 1
#define OVE_LNX_DEVW_WRITE 2
#define OVE_LNX_DEVW_IOCTL 3

#define OVE_LNX_NDEV 16	    /* max registered device nodes */
#define OVE_LNX_NDEVOPEN 16 /* max concurrent device opens (pooled) */
#define OVE_LNX_NDEVTICK 4  /* max coordinator-tick callbacks (fb flush, touch poll) */

static struct ove_lnx_dev g_lnx_devs[OVE_LNX_NDEV];
static int g_lnx_ndev;
static struct ove_lnx_dev_open g_lnx_devopen[OVE_LNX_NDEVOPEN];
static void (*g_lnx_devtick[OVE_LNX_NDEVTICK])(uint64_t now_us);
static int g_lnx_ndevtick;

/* ---- registration ---------------------------------------------------------- */
int ove_lnx_dev_register(const struct ove_lnx_dev *dev)
{
	if (!dev || !dev->path || !dev->ops)
		return -OVE_LNX_EINVAL;
	if (g_lnx_ndev >= OVE_LNX_NDEV)
		return -OVE_LNX_EMFILE;
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
void ove_lnx_dev_tick_register(void (*fn)(uint64_t now_us));
void ove_lnx_dev_tick_register(void (*fn)(uint64_t now_us))
{
	if (fn && g_lnx_ndevtick < OVE_LNX_NDEVTICK)
		g_lnx_devtick[g_lnx_ndevtick++] = fn;
}

int ove_lnx_dev_lookup(const char *abspath)
{
	for (int i = 0; i < g_lnx_ndev; i++)
		if (strcmp(g_lnx_devs[i].path, abspath) == 0)
			return i;
	return -1;
}

int ove_lnx_dev_count(void)
{
	return g_lnx_ndev;
}

const char *ove_lnx_dev_path(int i, uint32_t *mode)
{
	if (i < 0 || i >= g_lnx_ndev)
		return NULL;
	if (mode)
		*mode = OVE_LNX_S_IFCHR | 0666u;
	return g_lnx_devs[i].path;
}

/* ---- open pool ------------------------------------------------------------- */
static struct ove_lnx_dev_open *open_slot(int oi)
{
	if (oi < 0 || oi >= OVE_LNX_NDEVOPEN || !g_lnx_devopen[oi].used)
		return NULL;
	return &g_lnx_devopen[oi];
}

long ove_lnx_dev_open_new(ove_lnx_proc_t *p, int devidx, int flags)
{
	(void)p;
	if (devidx < 0 || devidx >= g_lnx_ndev)
		return -OVE_LNX_ENOENT;
	int oi = -1;
	for (int i = 0; i < OVE_LNX_NDEVOPEN; i++)
		if (!g_lnx_devopen[i].used) {
			oi = i;
			break;
		}
	if (oi < 0)
		return -OVE_LNX_EMFILE;
	struct ove_lnx_dev_open *o = &g_lnx_devopen[oi];
	memset(o, 0, sizeof(*o));
	o->used = 1;
	o->refs = 1;
	o->dev = (uint8_t)devidx;
	o->oflags = (uint16_t)flags;
	struct ove_lnx_dev *d = &g_lnx_devs[devidx];
	if (d->ops->open) {
		long r = d->ops->open(d, o, flags);
		if (r < 0) {
			o->used = 0;
			return r;
		}
	}
	return oi;
}

void ove_lnx_dev_get(int oi)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (o && o->refs < 0xff)
		o->refs++;
}

void ove_lnx_dev_setfl(int oi, int flags)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (o)
		o->oflags = (uint16_t)flags;
}

int ove_lnx_dev_getfl(int oi)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	return o ? o->oflags : 0;
}

void ove_lnx_dev_close(int oi)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return;
	if (o->refs > 1) {
		o->refs--;
		return;
	}
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (d->ops->release)
		d->ops->release(d, o);
	o->used = 0;
}

/* ---- read / write / ioctl (with deferred-block park) ----------------------- */
long ove_lnx_dev_read(ove_lnx_proc_t *p, int oi, void *buf, size_t len)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->read)
		return -OVE_LNX_EINVAL;
	long r = d->ops->read(d, o, p, buf, len);
	if (r == -OVE_LNX_EAGAIN) {
		if (o->oflags & OVE_LNX_O_NONBLOCK)
			return -OVE_LNX_EAGAIN;
		p->dev_wait = OVE_LNX_DEVW_READ; /* park: the coordinator retries */
		p->dev_oi = oi;
		p->dev_buf = (uintptr_t)buf;
		p->dev_len = len;
		return 0;
	}
	return r;
}

long ove_lnx_dev_write(ove_lnx_proc_t *p, int oi, const void *buf, size_t len)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->write)
		return -OVE_LNX_EINVAL;
	long r = d->ops->write(d, o, p, buf, len);
	if (r == -OVE_LNX_EAGAIN) {
		if (o->oflags & OVE_LNX_O_NONBLOCK)
			return -OVE_LNX_EAGAIN;
		p->dev_wait = OVE_LNX_DEVW_WRITE;
		p->dev_oi = oi;
		p->dev_buf = (uintptr_t)buf;
		p->dev_len = len;
		return 0;
	}
	return r;
}

long ove_lnx_dev_ioctl(ove_lnx_proc_t *p, int oi, unsigned long cmd, unsigned long arg)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->ioctl)
		return -OVE_LNX_ENOTTY;
	long r = d->ops->ioctl(d, o, p, cmd, arg);
	if (r == -OVE_LNX_EAGAIN) {
		if (o->oflags & OVE_LNX_O_NONBLOCK)
			return -OVE_LNX_EAGAIN;
		p->dev_wait = OVE_LNX_DEVW_IOCTL;
		p->dev_oi = oi;
		p->dev_cmd = cmd;
		p->dev_buf = arg;
		return 0;
	}
	return r;
}

/* Positioned I/O: drive the same read/write op at `off` with the fd cursor
 * preserved (pread/pwrite semantics). The fb is inline (never -EAGAIN), so this
 * does not park; a blocking device would need the dev_wait path instead. */
long ove_lnx_dev_pread(ove_lnx_proc_t *p, int oi, void *buf, size_t len, uint32_t off)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->read)
		return -OVE_LNX_EINVAL;
	uint32_t save = o->pos;
	o->pos = off;
	long r = d->ops->read(d, o, p, buf, len);
	o->pos = save;
	return r;
}

long ove_lnx_dev_pwrite(ove_lnx_proc_t *p, int oi, const void *buf, size_t len, uint32_t off)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (!d->ops->write)
		return -OVE_LNX_EINVAL;
	uint32_t save = o->pos;
	o->pos = off;
	long r = d->ops->write(d, o, p, buf, len);
	o->pos = save;
	return r;
}

unsigned ove_lnx_dev_poll(int oi)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return 0;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	return d->ops->poll ? d->ops->poll(d, o) : (OVE_LNX_POLLIN | OVE_LNX_POLLOUT);
}

long ove_lnx_dev_lseek(int oi, long off, int whence)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (d->size == 0)
		return -OVE_LNX_ESPIPE; /* a non-seekable device (no fixed extent) */
	long base;
	switch (whence) {
	case OVE_LNX_SEEK_SET:
		base = 0;
		break;
	case OVE_LNX_SEEK_CUR:
		base = (long)o->pos;
		break;
	case OVE_LNX_SEEK_END:
		base = (long)d->size;
		break;
	default:
		return -OVE_LNX_EINVAL;
	}
	long pos = base + off;
	if (pos < 0 || pos > (long)d->size)
		return -OVE_LNX_EINVAL;
	o->pos = (uint32_t)pos;
	return pos;
}

/* ---- stat / getdents helpers ----------------------------------------------- */
void ove_lnx_dev_fstat(int oi, uint32_t *mode, uint64_t *rdev, uint64_t *size)
{
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o) {
		if (mode)
			*mode = OVE_LNX_S_IFCHR | 0666u;
		if (rdev)
			*rdev = 0;
		if (size)
			*size = 0;
		return;
	}
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	if (mode)
		*mode = OVE_LNX_S_IFCHR | 0666u;
	if (rdev)
		*rdev = ((uint64_t)d->major << 8) | d->minor;
	if (size)
		*size = d->size;
}

int ove_lnx_dev_stat_path(const char *abspath, uint32_t *mode, uint64_t *rdev)
{
	int di = ove_lnx_dev_lookup(abspath);
	if (di < 0)
		return -1;
	struct ove_lnx_dev *d = &g_lnx_devs[di];
	if (mode)
		*mode = OVE_LNX_S_IFCHR | 0666u;
	if (rdev)
		*rdev = ((uint64_t)d->major << 8) | d->minor;
	return 0;
}

/* ---- coordinator: retry parked device I/O + periodic tick ------------------ */
long ove_lnx_dev_retry(ove_lnx_proc_t *p)
{
	int oi = p->dev_oi;
	struct ove_lnx_dev_open *o = open_slot(oi);
	if (!o)
		return -OVE_LNX_EBADF;
	struct ove_lnx_dev *d = &g_lnx_devs[o->dev];
	switch (p->dev_wait) {
	case OVE_LNX_DEVW_READ:
		return d->ops->read ? d->ops->read(d, o, p, (void *)p->dev_buf, p->dev_len)
				    : -OVE_LNX_EINVAL;
	case OVE_LNX_DEVW_WRITE:
		return d->ops->write ? d->ops->write(d, o, p, (const void *)p->dev_buf, p->dev_len)
				     : -OVE_LNX_EINVAL;
	case OVE_LNX_DEVW_IOCTL:
		return d->ops->ioctl ? d->ops->ioctl(d, o, p, p->dev_cmd, p->dev_buf)
				     : -OVE_LNX_ENOTTY;
	default:
		return -OVE_LNX_EINVAL;
	}
}

void ove_lnx_dev_tick(uint64_t now_us)
{
	for (int i = 0; i < g_lnx_ndevtick; i++)
		g_lnx_devtick[i](now_us);
}

/* ---- fork / exit fd lifecycle ---------------------------------------------- */
void ove_lnx_dev_fork_inherit(ove_lnx_proc_t *child)
{
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++)
		if (child->fds[fd].kind == OVE_LNX_FD_DEV)
			ove_lnx_dev_get(child->fds[fd].file_idx);
}

void ove_lnx_dev_proc_exit(ove_lnx_proc_t *p)
{
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++)
		if (p->fds[fd].kind == OVE_LNX_FD_DEV) {
			ove_lnx_dev_close(p->fds[fd].file_idx);
			p->fds[fd].kind = 0; /* FD_FREE (private to the syscall layer) */
		}
}

/* ---- Kconfig-auto class registration --------------------------------------- */
/* Each class driver (fb, input, ...) provides ove_lnx_dev_autoreg_<c>() behind its
 * CONFIG_OVE_LINUX_DEV_<C>. Gate the CALLS on the same config rather than relying on
 * weak no-op fallbacks: a weak fallback here would be bound to the same-TU definition
 * by GCC's default -fno-semantic-interposition, and the class object — reachable only
 * through this hook — would never be pulled from the archive (so /dev/fb0 /
 * /dev/input/event0 would silently not register on an archive+GC link, e.g. NuttX).
 * Gating makes the call a direct reference to the compiled class's strong definition.
 * Run once on the coordinator thread (blocking HAL init — ove_fb_init / ove_i2c_create
 * — is legal there). */
#if defined(CONFIG_OVE_LINUX_DEV_FB)
void ove_lnx_dev_autoreg_fb(void);
#endif
#if defined(CONFIG_OVE_LINUX_DEV_INPUT)
void ove_lnx_dev_autoreg_input(void);
#endif

void ove_lnx_dev_autoreg_all(void)
{
#if defined(CONFIG_OVE_LINUX_DEV_FB)
	ove_lnx_dev_autoreg_fb();
#endif
#if defined(CONFIG_OVE_LINUX_DEV_INPUT)
	ove_lnx_dev_autoreg_input();
#endif
}

/* Weak input feeder so the core links before the evdev class (P4) defines it. */
__attribute__((weak)) void ove_lnx_input_report_touch(int x, int y, int pressed)
{
	(void)x;
	(void)y;
	(void)pressed;
}

/* Weak coordinator kick: the run loop provides the strong version (posts its
 * event). The host cmocka test links the core without the run loop, so this
 * no-op keeps a driver's ove_lnx_dev_kick() call resolvable there. */
__attribute__((weak)) void ove_lnx_dev_kick(void) {}

#endif /* CONFIG_OVE_LINUX_DEV */
