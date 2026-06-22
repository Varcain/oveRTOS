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
#define OVE_LNX_NR_open 5
#define OVE_LNX_NR_close 6
#define OVE_LNX_NR_execve 11
#define OVE_LNX_NR_lseek 19
#define OVE_LNX_NR_getpid 20
#define OVE_LNX_NR_brk 45
#define OVE_LNX_NR_ioctl 54
#define OVE_LNX_NR_munmap 91
#define OVE_LNX_NR_writev 146
#define OVE_LNX_NR_rt_sigprocmask 175
#define OVE_LNX_NR_mmap2 192
#define OVE_LNX_NR_fstat64 197
#define OVE_LNX_NR_getuid32 199
#define OVE_LNX_NR_getgid32 200
#define OVE_LNX_NR_geteuid32 201
#define OVE_LNX_NR_getegid32 202
#define OVE_LNX_NR_getdents64 217
#define OVE_LNX_NR_fcntl64 221
#define OVE_LNX_NR_exit_group 248
#define OVE_LNX_NR_set_tid_address 256
#define OVE_LNX_NR_openat 322
#define OVE_LNX_NR_set_robust_list 338
#define OVE_LNX_NR_statx 397

/* mmap flags (ARM). Only anonymous mappings are backed (from the arena). */
#define OVE_LNX_MAP_ANONYMOUS 0x20

/* open(2) flags: low two bits select the access mode (read-only filesystem). */
#define OVE_LNX_O_ACCMODE 0x3
#define OVE_LNX_O_RDONLY 0x0
/* openat dirfd sentinel for the current working directory. */
#define OVE_LNX_AT_FDCWD (-100)
/* lseek(2) whence. */
#define OVE_LNX_SEEK_SET 0
#define OVE_LNX_SEEK_CUR 1
#define OVE_LNX_SEEK_END 2
/* struct stat st_mode file-type bits. */
#define OVE_LNX_S_IFMT 0xf000u
#define OVE_LNX_S_IFREG 0x8000u
#define OVE_LNX_S_IFDIR 0x4000u
#define OVE_LNX_S_IFCHR 0x2000u
/* getdents64 d_type values. */
#define OVE_LNX_DT_DIR 4
#define OVE_LNX_DT_REG 8
/* statx: AT_EMPTY_PATH means "stat the dirfd itself" (fstat); the basic-stats
 * result mask reported back in stx_mask. */
#define OVE_LNX_AT_EMPTY_PATH 0x1000
#define OVE_LNX_STATX_BASIC_STATS 0x000007ffu

/* Linux errno values returned (negated) on syscall failure. */
#define OVE_LNX_ENOENT 2
#define OVE_LNX_EBADF 9
#define OVE_LNX_ENOMEM 12
#define OVE_LNX_EACCES 13
#define OVE_LNX_EFAULT 14
#define OVE_LNX_ENOTDIR 20
#define OVE_LNX_EISDIR 21
#define OVE_LNX_EMFILE 24
#define OVE_LNX_ENOTTY 25
#define OVE_LNX_ESPIPE 29
#define OVE_LNX_EROFS 30
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

/** One node in the read-only in-memory rootfs (a flat path → bytes table). */
typedef struct ove_lnx_file {
	const char *path;    /**< Absolute path, e.g. "/etc/hostname". */
	const uint8_t *data; /**< File contents (NULL for a directory). */
	size_t size;	     /**< Length in bytes (0 for a directory). */
	uint32_t mode;	     /**< st_mode; 0 means a regular file. Set @c OVE_LNX_S_IFDIR
			      *   for directories (their children are the entries one
			      *   path component below @c path). */
} ove_lnx_file_t;

/** Open-file-descriptor slot. */
typedef struct ove_lnx_fd {
	uint8_t kind;  /**< 0 = free, 1 = console (std stream), 2 = rootfs file. */
	int file_idx;  /**< Index into the rootfs table (kind == file). */
	size_t offset; /**< Read cursor (kind == file). */
} ove_lnx_fd_t;

/** Maximum simultaneously-open file descriptors per process. */
#define OVE_LNX_MAX_FDS 16
/** Bounds for an execve() argument vector captured for the engine to relaunch. */
#define OVE_LNX_EXEC_MAXARGS 8
#define OVE_LNX_EXEC_ARGBUF 256

/**
 * @brief A Linux process context — the state syscalls act on.
 *
 * NOMMU model: a bounded program break + anonymous mmap carved from an
 * @c ove_arena, a small fd table over standard streams (caller callbacks) and a
 * read-only in-memory rootfs, and an exit latch. Signals / a writable VFS /
 * fork+exec land in later phases.
 */
typedef struct ove_lnx_proc {
	ove_arena_t *arena;		   /**< Backs @c brk and anonymous @c mmap. */
	uintptr_t brk_base;		   /**< Initial program break. */
	uintptr_t brk_cur;		   /**< Current program break. */
	uintptr_t brk_max;		   /**< Ceiling imposed by the arena reservation. */
	ove_lnx_write_fn write_fn;	   /**< fd 1/2 sink; NULL → @c -OVE_LNX_EBADF. */
	ove_lnx_read_fn read_fn;	   /**< fd 0 source; NULL → EOF. */
	void *io_ctx;			   /**< Opaque, passed to @c write_fn / @c read_fn. */
	const ove_lnx_file_t *fs;	   /**< Read-only rootfs table (NULL → no files). */
	int fs_count;			   /**< Number of entries in @c fs. */
	ove_lnx_fd_t fds[OVE_LNX_MAX_FDS]; /**< fd table; 0/1/2 are the std streams. */
	int exited;			   /**< Set once @c exit / @c exit_group is called. */
	int exit_status;		   /**< Low 8 bits of the exit code. */
	/* execve request: the engine seam relaunches the thread on this rootfs
	 * program with the captured argument vector (image replacement). */
	int exec_pending;			 /**< Set when execve() should relaunch. */
	int exec_file_idx;			 /**< Rootfs index of the program to run. */
	int exec_argc;				 /**< Captured argument count. */
	char *exec_argv[OVE_LNX_EXEC_MAXARGS];	 /**< Captured argv (into exec_argv_buf). */
	char exec_argv_buf[OVE_LNX_EXEC_ARGBUF]; /**< Backing store for exec_argv. */
} ove_lnx_proc_t;

/**
 * @brief Attach a read-only in-memory rootfs the program can @c open / @c read.
 * @note Requires @c CONFIG_OVE_LINUX.
 */
void ove_lnx_proc_set_rootfs(ove_lnx_proc_t *proc, const ove_lnx_file_t *files, int count);

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

/* ELF auxiliary-vector types in the startup block (uClibc scans them after envp). */
#define OVE_LNX_AT_NULL 0
#define OVE_LNX_AT_PAGESZ 6
#define OVE_LNX_AT_RANDOM 25

/**
 * @brief Build a uClinux/bFLT process stack for a loaded program's crt0.
 *
 * Lays out, at the top of @p stack, the @c flat_argvp_envp_on_stack startup
 * block an @c elf2flt crt0 reads on ARM: @c sp[0]=argc, @c sp[1]=argv (a pointer
 * to the argv array), @c sp[2]=envp (a pointer to the envp array), followed by
 * the NULL-terminated @c argv[] and @c envp[] arrays, a minimal auxv
 * (@c AT_PAGESZ, @c AT_RANDOM, @c AT_NULL), and the argument/environment strings.
 * The header is NOT the ELF inline layout, but @c __uClibc_main still scans for
 * an auxv right after the envp array, so a terminated one must be present.
 * The returned pointer is the initial stack pointer (8-byte aligned, pointing at
 * @c argc) to hand the program entry.
 *
 * @param[in] stack      Base of the stack region.
 * @param[in] stack_size Size of the stack region in bytes.
 * @param[in] argc       Argument count (<= a small internal bound).
 * @param[in] argv       @p argc argument strings.
 * @param[in] envp       NULL-terminated environment strings (may be NULL).
 * @return The initial stack pointer, or NULL on bad arguments / insufficient room.
 * @note Requires @c CONFIG_OVE_LINUX.
 */
void *ove_lnx_setup_stack(void *stack, size_t stack_size, int argc, const char *const argv[],
			  const char *const envp[]);

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
