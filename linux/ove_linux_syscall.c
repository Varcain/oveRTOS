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

/* Bound on argv/envp entries the startup stack will lay out. */
#define OVE_LNX_MAX_VEC 32

void *ove_lnx_setup_stack(void *stack, size_t stack_size, int argc, const char *const argv[],
			  const char *const envp[])
{
	if (!stack || !argv || argc < 0 || argc > OVE_LNX_MAX_VEC)
		return NULL;

	int envc = 0;
	while (envp && envp[envc])
		envc++;
	if (envc > OVE_LNX_MAX_VEC)
		return NULL;

	uintptr_t argp[OVE_LNX_MAX_VEC];
	uintptr_t envpp[OVE_LNX_MAX_VEC];
	uint8_t *sp = (uint8_t *)stack + stack_size;
	uint8_t *floor = (uint8_t *)stack;

	/* Copy env then arg strings to the top of the stack, recording addresses. */
	for (int i = envc - 1; i >= 0; i--) {
		size_t n = strlen(envp[i]) + 1;
		if (sp - n < floor)
			return NULL;
		sp -= n;
		memcpy(sp, envp[i], n);
		envpp[i] = (uintptr_t)sp;
	}
	for (int i = argc - 1; i >= 0; i--) {
		size_t n = strlen(argv[i]) + 1;
		if (sp - n < floor)
			return NULL;
		sp -= n;
		memcpy(sp, argv[i], n);
		argp[i] = (uintptr_t)sp;
	}

	/* 16 bytes for AT_RANDOM (stack-canary seed; not cryptographic here). */
	if (sp - 16 < floor)
		return NULL;
	sp -= 16;
	uint8_t *rnd = sp;
	for (int i = 0; i < 16; i++)
		rnd[i] = (uint8_t)(0xa5u ^ (unsigned)i);

	/*
	 * uClinux/bFLT (flat_argvp_envp_on_stack, used on ARM) layout — NOT the
	 * ELF inline layout: the kernel passes the argv/envp array *pointers* on
	 * the stack, so an elf2flt crt0 reads sp[0]=argc, sp[1]=argv, sp[2]=envp.
	 * Below the strings lay the 3-word header, the argv[] and envp[] arrays it
	 * points at, then a terminated auxv — __uClibc_main scans for one right
	 * after the envp array, and unterminated garbage there crashes it.
	 */
	size_t nwords = 3 + (size_t)argc + 1 + (size_t)envc + 1 + 6;
	uintptr_t *hdr =
		(uintptr_t *)((uintptr_t)(sp - nwords * sizeof(uintptr_t)) & ~(uintptr_t)7);
	if ((uint8_t *)hdr < floor)
		return NULL;

	uintptr_t *argv_arr = hdr + 3;
	uintptr_t *envp_arr = argv_arr + (size_t)argc + 1;
	uintptr_t *auxv = envp_arr + (size_t)envc + 1;
	hdr[0] = (uintptr_t)argc;
	hdr[1] = (uintptr_t)argv_arr;
	hdr[2] = (uintptr_t)envp_arr;
	for (int i = 0; i < argc; i++)
		argv_arr[i] = argp[i];
	argv_arr[argc] = 0;
	for (int i = 0; i < envc; i++)
		envp_arr[i] = envpp[i];
	envp_arr[envc] = 0;
	auxv[0] = OVE_LNX_AT_PAGESZ;
	auxv[1] = 4096;
	auxv[2] = OVE_LNX_AT_RANDOM;
	auxv[3] = (uintptr_t)rnd;
	auxv[4] = OVE_LNX_AT_NULL;
	auxv[5] = 0;

	return hdr; /* initial SP, pointing at argc */
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

/*
 * Anonymous mmap, backed by the process arena (uClibc's malloc uses it for
 * larger allocations). File mappings need a VFS and are not supported yet.
 */
static long sys_mmap2(ove_lnx_proc_t *p, uintptr_t addr, size_t len, int prot, int flags, int fd)
{
	(void)addr;
	(void)prot;
	if (!(flags & OVE_LNX_MAP_ANONYMOUS) || fd >= 0)
		return -OVE_LNX_ENOSYS;
	if (len == 0)
		return -OVE_LNX_EINVAL;

	void *m = ove_arena_alloc(p->arena, len);
	if (!m)
		return -OVE_LNX_ENOMEM;
	memset(m, 0, len); /* anonymous memory reads as zero */
	return (long)(uintptr_t)m;
}

/*
 * munmap is a no-op for now: the bump/free-list arena is reclaimed wholesale at
 * process teardown, and a partial unmap of an arena chunk could corrupt the
 * free list. Tracking mmap extents for precise release is a later step.
 */
static long sys_munmap(ove_lnx_proc_t *p, uintptr_t addr, size_t len)
{
	(void)p;
	(void)addr;
	(void)len;
	return 0;
}

long ove_lnx_syscall(ove_lnx_proc_t *proc, long nr, long a0, long a1, long a2, long a3, long a4,
		     long a5)
{
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
	case OVE_LNX_NR_mmap2:
		return sys_mmap2(proc, (uintptr_t)a0, (size_t)a1, (int)a2, (int)a3, (int)a4);
	case OVE_LNX_NR_munmap:
		return sys_munmap(proc, (uintptr_t)a0, (size_t)a1);
	case OVE_LNX_NR_exit:
	case OVE_LNX_NR_exit_group:
		return sys_exit(proc, (int)a0);
	/* libc-init / identity stubs: enough for a static uClibc program to start. */
	case OVE_LNX_NR_getpid:
		return 1;
	case OVE_LNX_NR_getuid32:
	case OVE_LNX_NR_geteuid32:
	case OVE_LNX_NR_getgid32:
	case OVE_LNX_NR_getegid32:
		return 0; /* run as root */
	case OVE_LNX_NR_ioctl:
		return -OVE_LNX_ENOTTY; /* no tty: stdio falls back to block buffering */
	case OVE_LNX_NR_rt_sigprocmask:
		return 0;
	case OVE_LNX_NR_set_tid_address:
		return 1; /* our single thread's tid */
	case OVE_LNX_NR_set_robust_list:
		return 0;
	default:
		return -OVE_LNX_ENOSYS;
	}
}

#endif /* CONFIG_OVE_LINUX */
