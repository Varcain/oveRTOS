/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include "ove/linux/syscall.h"

#include <string.h>

/*
 * Linux syscall personality — engine-agnostic dispatch.
 *
 * Translates the Linux syscall ABI into oveRTOS primitives. The trap frame is
 * decoded by the per-engine SVC seam, which calls ove_lnx_syscall() with the
 * register arguments; this file owns the syscall table and the process state
 * those syscalls mutate. Pointer arguments are program addresses — in the flat
 * (NOMMU) model the program shares our address space, so they are used
 * directly after a NULL check (a future MMU tier would translate them).
 */

int ove_lnx_proc_init(ove_lnx_proc_t *proc, ove_arena_t *arena, size_t brk_bytes)
{
	if (!proc || !arena)
		return OVE_ERR_INVALID_PARAM;

	memset(proc, 0, sizeof(*proc));
	proc->arena = arena;
	if (brk_bytes) {
		void *brk = ove_arena_alloc(arena, brk_bytes);
		if (!brk)
			return OVE_ERR_NO_MEMORY;
		proc->brk_base = (uintptr_t)brk;
		proc->brk_cur = proc->brk_base;
		proc->brk_max = proc->brk_base + brk_bytes;
	}
	return OVE_OK;
}

static long sys_write(ove_lnx_proc_t *p, int fd, const void *buf, size_t len)
{
	if (fd != 1 && fd != 2)
		return -OVE_LNX_EBADF;
	if (!p->write_fn)
		return -OVE_LNX_EBADF;
	if (len && !buf)
		return -OVE_LNX_EFAULT;
	return p->write_fn(p->io_ctx, fd, buf, len);
}

static long sys_writev(ove_lnx_proc_t *p, int fd, const ove_lnx_iovec *iov, int iovcnt)
{
	if (fd != 1 && fd != 2)
		return -OVE_LNX_EBADF;
	if (iovcnt < 0)
		return -OVE_LNX_EINVAL;
	if (iovcnt && !iov)
		return -OVE_LNX_EFAULT;

	long total = 0;
	for (int i = 0; i < iovcnt; i++) {
		if (iov[i].iov_len == 0)
			continue;
		long r = sys_write(p, fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total ? total : r;
		total += r;
		if ((size_t)r < iov[i].iov_len)
			break; /* short write */
	}
	return total;
}

static long sys_read(ove_lnx_proc_t *p, int fd, void *buf, size_t len)
{
	if (fd != 0)
		return -OVE_LNX_EBADF;
	if (!p->read_fn)
		return 0; /* EOF */
	if (len && !buf)
		return -OVE_LNX_EFAULT;
	return p->read_fn(p->io_ctx, fd, buf, len);
}

static long sys_brk(ove_lnx_proc_t *p, uintptr_t addr)
{
	/* Linux brk: move the break to addr if valid, then return the (possibly
	 * unchanged) break. uClibc's sbrk detects failure by ret != requested. */
	if (addr >= p->brk_base && addr <= p->brk_max)
		p->brk_cur = addr;
	return (long)p->brk_cur;
}

static long sys_exit(ove_lnx_proc_t *p, int status)
{
	p->exited = 1;
	p->exit_status = status & 0xff;
	return 0;
}

long ove_lnx_syscall(ove_lnx_proc_t *proc, long nr, long a0, long a1, long a2, long a3, long a4,
		     long a5)
{
	(void)a3;
	(void)a4;
	(void)a5;
	if (!proc)
		return -OVE_LNX_EINVAL;

	switch (nr) {
	case OVE_LNX_NR_read:
		return sys_read(proc, (int)a0, (void *)(uintptr_t)a1, (size_t)a2);
	case OVE_LNX_NR_write:
		return sys_write(proc, (int)a0, (const void *)(uintptr_t)a1, (size_t)a2);
	case OVE_LNX_NR_writev:
		return sys_writev(proc, (int)a0, (const ove_lnx_iovec *)(uintptr_t)a1, (int)a2);
	case OVE_LNX_NR_brk:
		return sys_brk(proc, (uintptr_t)a0);
	case OVE_LNX_NR_exit:
	case OVE_LNX_NR_exit_group:
		return sys_exit(proc, (int)a0);
	default:
		return -OVE_LNX_ENOSYS;
	}
}

#endif /* CONFIG_OVE_LINUX */
