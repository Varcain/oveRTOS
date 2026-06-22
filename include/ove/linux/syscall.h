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
#define OVE_LNX_NR_fork 2
#define OVE_LNX_NR_read 3
#define OVE_LNX_NR_dup 41
#define OVE_LNX_NR_pipe 42
#define OVE_LNX_NR_dup2 63
#define OVE_LNX_NR_kill 37
#define OVE_LNX_NR_sigreturn 119
#define OVE_LNX_NR_dup3 358
#define OVE_LNX_NR_rt_sigreturn 173
#define OVE_LNX_NR_gettid 224
#define OVE_LNX_NR_tkill 238
#define OVE_LNX_NR_tgkill 268
#define OVE_LNX_NR_write 4
#define OVE_LNX_NR_open 5
#define OVE_LNX_NR_close 6
#define OVE_LNX_NR_execve 11
#define OVE_LNX_NR_lseek 19
#define OVE_LNX_NR_getpid 20
#define OVE_LNX_NR_setpgid 57
#define OVE_LNX_NR_getppid 64
#define OVE_LNX_NR_wait4 114
#define OVE_LNX_NR_uname 122
#define OVE_LNX_NR_poll 168
#define OVE_LNX_NR_ppoll_time64 414
#define OVE_LNX_NR_brk 45
#define OVE_LNX_NR_ioctl 54
#define OVE_LNX_NR_munmap 91
#define OVE_LNX_NR_writev 146
#define OVE_LNX_NR_prctl 172
#define OVE_LNX_NR_rt_sigaction 174
#define OVE_LNX_NR_rt_sigprocmask 175
#define OVE_LNX_NR_getcwd 183
#define OVE_LNX_NR_vfork 190
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
#define OVE_LNX_S_IFLNK 0xa000u
/* getdents64 d_type values. */
#define OVE_LNX_DT_DIR 4
#define OVE_LNX_DT_REG 8
/* termios ioctls so a console looks like a tty (isatty → interactive shell). */
#define OVE_LNX_TCGETS 0x5401
#define OVE_LNX_TCSETS 0x5402
#define OVE_LNX_TCSETSW 0x5403
#define OVE_LNX_TCSETSF 0x5404
#define OVE_LNX_TIOCGWINSZ 0x5413
/* c_lflag/c_iflag/c_oflag/c_cflag bits used for the canonical-tty default. */
#define OVE_LNX_ISIG 0x0001u
#define OVE_LNX_ICANON 0x0002u
#define OVE_LNX_ECHO 0x0008u
#define OVE_LNX_ICRNL 0x0100u
#define OVE_LNX_OPOST 0x0001u
#define OVE_LNX_ONLCR 0x0004u
#define OVE_LNX_CS8 0x0030u
#define OVE_LNX_CREAD 0x0080u
/* Signals: a per-process disposition table + the handful the shell cares about.
 * SIG_DFL/SIG_IGN are the special handler sentinels. */
#define OVE_LNX_NSIG 32
#define OVE_LNX_SIG_DFL 0
#define OVE_LNX_SIG_IGN 1
#define OVE_LNX_SIGINT 2
#define OVE_LNX_SIGQUIT 3
#define OVE_LNX_SIGABRT 6
#define OVE_LNX_SIGKILL 9
#define OVE_LNX_SIGSEGV 11
#define OVE_LNX_SIGTERM 15
/* fcntl commands: F_DUPFD duplicates an fd (the shell dups stdin for its
 * interactive fd); the rest are benign get/set probes. */
#define OVE_LNX_F_DUPFD 0
#define OVE_LNX_F_GETFD 1
#define OVE_LNX_F_SETFD 2
#define OVE_LNX_F_GETFL 3
#define OVE_LNX_F_SETFL 4
#define OVE_LNX_F_DUPFD_CLOEXEC 1030
/* c_cc indices (Linux generic, NCCS=19). */
#define OVE_LNX_VINTR 0
#define OVE_LNX_VERASE 2
#define OVE_LNX_VEOF 4
#define OVE_LNX_VMIN 6
#define OVE_LNX_NCCS 19
/* statx: AT_EMPTY_PATH means "stat the dirfd itself" (fstat); the basic-stats
 * result mask reported back in stx_mask. */
#define OVE_LNX_AT_EMPTY_PATH 0x1000
#define OVE_LNX_STATX_BASIC_STATS 0x000007ffu

/* Linux errno values returned (negated) on syscall failure. */
#define OVE_LNX_ENOENT 2
#define OVE_LNX_EINTR 4
#define OVE_LNX_EBADF 9
#define OVE_LNX_ECHILD 10
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
#define OVE_LNX_ERANGE 34
#define OVE_LNX_ENOSYS 38

/** Scatter/gather element, matching the target's @c struct iovec layout. */
typedef struct ove_lnx_iovec {
	void *iov_base; /**< Start of the buffer (in the program's address space). */
	size_t iov_len; /**< Length of the buffer in bytes. */
} ove_lnx_iovec;

/** Kernel @c struct termios (ARM, NCCS=19), filled by the TCGETS ioctl. */
typedef struct ove_lnx_termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_line;
	uint8_t c_cc[OVE_LNX_NCCS];
} ove_lnx_termios;

/** @c struct winsize returned by TIOCGWINSZ. */
typedef struct ove_lnx_winsize {
	uint16_t ws_row;
	uint16_t ws_col;
	uint16_t ws_xpixel;
	uint16_t ws_ypixel;
} ove_lnx_winsize;

/** @c struct pollfd for poll(2). */
typedef struct ove_lnx_pollfd {
	int fd;
	short events;
	short revents;
} ove_lnx_pollfd;
#define OVE_LNX_POLLIN 0x0001
#define OVE_LNX_POLLOUT 0x0004

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
	uint8_t kind;  /**< 0 = free, 1 = console, 2 = rootfs file, 3 = pipe. */
	uint8_t rw;    /**< pipe end: 0 = read, 1 = write (kind == pipe). */
	int file_idx;  /**< rootfs index (kind == file) or pipe index (kind == pipe). */
	size_t offset; /**< Read cursor (kind == file). */
} ove_lnx_fd_t;

/** Maximum simultaneously-open file descriptors per process. */
#define OVE_LNX_MAX_FDS 16
/** Max exited children queued for wait4 (a pipeline forks several). */
#define OVE_LNX_MAX_CHILD 8
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
	int pid;			   /**< This process's id (1 for the initial program). */
	int ppid;			   /**< Parent process id (0 for the initial program). */
	int exited;			   /**< Set once @c exit / @c exit_group is called. */
	int exit_status;		   /**< Low 8 bits of the exit code. */
	/* Queue of exited (zombie) children awaiting wait4, FIFO. A pipeline forks
	 * more than one child, so a single slot is not enough. */
	int child_pid[OVE_LNX_MAX_CHILD];    /**< pids of exited children. */
	int child_status[OVE_LNX_MAX_CHILD]; /**< their exit codes. */
	int child_count;		     /**< number queued. */
	/* Signal disposition: per-signal handler address (or SIG_DFL/SIG_IGN) and
	 * the libc-supplied sa_restorer the engine returns to after the handler. */
	uintptr_t sig_handler[OVE_LNX_NSIG];
	uintptr_t sig_restorer[OVE_LNX_NSIG];
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
 * @brief Parse a newc CPIO archive (e.g. a Buildroot rootfs.cpio) into a rootfs
 *        table usable by @ref ove_lnx_proc_set_rootfs.
 *
 * Each entry's relative name gets a leading "/" written into @p namebuf (a
 * leading "./" is stripped); regular-file @c data points into @p cpio in place.
 * Stops at the "TRAILER!!!" entry.
 *
 * @return number of entries, or -1 on malformed input / table-or-namebuf overflow.
 * @note Requires @c CONFIG_OVE_LINUX.
 */
int ove_lnx_cpio_to_rootfs(const uint8_t *cpio, size_t len, ove_lnx_file_t *out, int max_entries,
			   char *namebuf, size_t namebuf_len);

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
