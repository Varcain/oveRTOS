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
 * @brief Linux syscall dispatch for loaded FDPIC programs.
 *
 * The engine-agnostic core of the oveRTOS Linux personality: it impersonates
 * the Linux kernel's syscall ABI for stock uClibc-ng binaries. A per-engine
 * SVC trap (e.g. @c backends/nuttx/nuttx_lnx_trap.c) decodes the trap frame and
 * calls @c lxp_syscall(); this layer translates the call into oveRTOS
 * primitives and bounded process state. It neither installs the trap nor
 * touches memory protection — that is the engine seam's job.
 *
 * Scope (Phase A start): a minimal syscall set — @c write / @c writev /
 * @c read / @c brk / @c exit / @c exit_group — backed by a caller-provided I/O
 * sink and a bounded @c ove_arena program break. Unknown syscalls return
 * @c -LXP_ENOSYS.
 *
 * @note Requires @c LXP_ENABLE_LINUX.
 * @{
 */

#include <stddef.h>
#include <stdint.h>

#include "lxp/lxp_arena.h"
#include "lxp/lxp_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Linux ARM EABI syscall numbers (subset). */
#define LXP_NR_exit 1
#define LXP_NR_fork 2
#define LXP_NR_read 3
#define LXP_NR_dup 41
#define LXP_NR_pipe 42
#define LXP_NR_pipe2 359
#define LXP_NR_fcntl 55
#define LXP_NR_dup2 63
#define LXP_NR_kill 37
#define LXP_NR_sigreturn 119
#define LXP_NR_sched_yield 158
#define LXP_NR_eventfd2 356
#define LXP_NR_dup3 358
#define LXP_NR_rt_sigreturn 173
#define LXP_NR_gettid 224
#define LXP_NR_tkill 238
#define LXP_NR_tgkill 268
#define LXP_NR_write 4
#define LXP_NR_open 5
#define LXP_NR_close 6
#define LXP_NR_chdir 12
#define LXP_NR_execve 11
#define LXP_NR_lseek 19
#define LXP_NR__llseek 140
#define LXP_NR_ftruncate64 194
#define LXP_NR_getpid 20
#define LXP_NR_setpgid 57
#define LXP_NR_getppid 64
#define LXP_NR_wait4 114
#define LXP_NR_uname 122
#define LXP_NR_poll 168
#define LXP_NR_pselect6_time64 413
#define LXP_NR_ppoll_time64 414
#define LXP_NR_brk 45
#define LXP_NR_ioctl 54
#define LXP_NR_munmap 91
#define LXP_NR_writev 146
#define LXP_NR_prctl 172
#define LXP_NR_rt_sigaction 174
#define LXP_NR_rt_sigprocmask 175
#define LXP_NR_rt_sigsuspend 179
#define LXP_NR_getcwd 183
#define LXP_NR_vfork 190
#define LXP_NR_mmap2 192
#define LXP_NR_mprotect 125 /* ld.so RELRO hardening — no-op on NOMMU */
#define LXP_NR_pread64 180  /* ld.so loads .so segments via positioned reads (NOMMU) */
#define LXP_NR_pwrite64 181 /* LVGL fbdev writes framebuffer scanlines via positioned writes */
#define LXP_NR_fstat64 197
#define LXP_NR_getuid32 199
#define LXP_NR_getgid32 200
#define LXP_NR_geteuid32 201
#define LXP_NR_getegid32 202
#define LXP_NR_getdents 141
#define LXP_NR_getdents64 217
#define LXP_NR_fcntl64 221
#define LXP_NR_exit_group 248
#define LXP_NR_set_tid_address 256
#define LXP_NR_openat 322
#define LXP_NR_set_robust_list 338
#define LXP_NR_futex 240
#define LXP_NR_futex_time64 421
#define LXP_NR_statx 397
/* path-based metadata: stat/lstat/readlink/access families. */
#define LXP_NR_access 33
#define LXP_NR_readlink 85
#define LXP_NR_stat64 195
#define LXP_NR_lstat64 196
#define LXP_NR_fstatat64 327
#define LXP_NR_readlinkat 332
#define LXP_NR_faccessat 334
#define LXP_NR_faccessat2 439
/* time: clock_gettime / gettimeofday / nanosleep (+ the time64 variants). */
#define LXP_NR_gettimeofday 78
#define LXP_NR_nanosleep 162
#define LXP_NR_clock_gettime 263
#define LXP_NR_clock_nanosleep 265
#define LXP_NR_clock_gettime64 403
#define LXP_NR_clock_nanosleep_time64 407
/* writable filesystem mutation. */
#define LXP_NR_unlink 10
#define LXP_NR_chmod 15
#define LXP_NR_rename 38
#define LXP_NR_mkdir 39
#define LXP_NR_rmdir 40
#define LXP_NR_symlink 83
#define LXP_NR_mkdirat 323
#define LXP_NR_unlinkat 328
#define LXP_NR_renameat 329
#define LXP_NR_symlinkat 331
#define LXP_NR_fchmodat 333
#define LXP_NR_utimensat 348
#define LXP_NR_renameat2 382
#define LXP_NR_utimensat_time64 412
/* mount (no-op) + statfs (synthetic, for df) + sysinfo (uptime/free). */
#define LXP_NR_sysinfo 116
#define LXP_NR_mount 21
#define LXP_NR_umount2 52
#define LXP_NR_statfs64 266
#define LXP_NR_fstatfs64 267
#define LXP_NR_getrandom 384
/* init / getty / login boot + shell job control. */
#define LXP_NR_sync 36
#define LXP_NR_times 43
#define LXP_NR_fsync 118
#define LXP_NR_fdatasync 148
#define LXP_NR_prlimit64 369
#define LXP_NR_umask 60
#define LXP_NR_getpgrp 65
#define LXP_NR_setsid 66
#define LXP_NR_reboot 88
#define LXP_NR_fchmod 94
#define LXP_NR_setitimer 104
#define LXP_NR_clone 120

/* clone(2) flags. CLONE_VM => the child shares the caller's address space — a pthread,
 * not a fork. The run loop co-runs such a child in the parent's region (see EV_FORK). */
#define LXP_CLONE_VM 0x00000100u
#define LXP_NR_setgroups32 206
#define LXP_NR_fchown32 207
#define LXP_NR_setuid32 213
#define LXP_NR_setgid32 214
/* setres/setre privilege-drop family (dropbear drops privileges after auth and aborts if it
 * fails); uid/gid are not enforced on this tier, so accepting them is inert. */
#define LXP_NR_setreuid32 203
#define LXP_NR_setregid32 204
#define LXP_NR_setresuid32 208
#define LXP_NR_getresuid32 209
#define LXP_NR_setresgid32 210
#define LXP_NR_getresgid32 211
#define LXP_AT_REMOVEDIR 0x200

/* Socket family (ARM EABI direct numbers; EABI does not use socketcall). */
#define LXP_NR_socket 281
#define LXP_NR_bind 282
#define LXP_NR_connect 283
#define LXP_NR_listen 284
#define LXP_NR_accept 285
#define LXP_NR_getsockname 286
#define LXP_NR_getpeername 287
#define LXP_NR_socketpair 288
#define LXP_NR_send 289
#define LXP_NR_sendto 290
#define LXP_NR_recv 291
#define LXP_NR_recvfrom 292
#define LXP_NR_shutdown 293
#define LXP_NR_setsockopt 294
#define LXP_NR_getsockopt 295
#define LXP_NR_sendmsg 296
#define LXP_NR_recvmsg 297
#define LXP_NR_accept4 366

/* mmap flags (ARM). Only anonymous mappings are backed (from the arena). */
#define LXP_MAP_ANONYMOUS 0x20

/* open(2) flags: low two bits select the access mode (read-only filesystem). */
#define LXP_O_ACCMODE 0x3
#define LXP_O_RDONLY 0x0
#define LXP_O_WRONLY 0x1
#define LXP_O_RDWR 0x2
#define LXP_O_CREAT 0x40
#define LXP_O_TRUNC 0x200
#define LXP_O_APPEND 0x400
#define LXP_O_NONBLOCK 0x800 /* a device open that returns -EAGAIN instead of blocking */
#define LXP_O_CLOEXEC 0x80000 /* close-on-exec (also pipe2/dup3/accept4 flag) */
#define LXP_FD_CLOEXEC 1	  /* fcntl(F_SETFD/F_GETFD) close-on-exec bit */
/* openat dirfd sentinel for the current working directory. */
#define LXP_AT_FDCWD (-100)
/* lseek(2) whence. */
#define LXP_SEEK_SET 0
#define LXP_SEEK_CUR 1
#define LXP_SEEK_END 2
/* struct stat st_mode file-type bits. */
#define LXP_S_IFMT 0xf000u
#define LXP_S_IFREG 0x8000u
#define LXP_S_IFDIR 0x4000u
#define LXP_S_IFCHR 0x2000u
#define LXP_S_IFLNK 0xa000u
#define LXP_S_IFSOCK 0xc000u
/* getdents64 d_type values. */
#define LXP_DT_CHR 2
#define LXP_DT_DIR 4
#define LXP_DT_REG 8
/* termios ioctls so a console looks like a tty (isatty → interactive shell). */
#define LXP_TCGETS 0x5401
#define LXP_TCSETS 0x5402
#define LXP_TCSETSW 0x5403
#define LXP_TCSETSF 0x5404
#define LXP_TIOCGWINSZ 0x5413
#define LXP_TIOCSWINSZ 0x5414
/* Unix98 pty-master ioctls (dropbear/openpty on /dev/ptmx): get the pts number and
 * lock/unlock the slave (grantpt/unlockpt). _IOR/_IOW('T',0x30/0x31) encodings. */
#define LXP_TIOCGPTN 0x80045430u
#define LXP_TIOCSPTLCK 0x40045431u
#define LXP_TIOCGPTPEER 0x5441
/* tty/session ioctls getty + login issue (accepted; we are a single console). */
#define LXP_TIOCSCTTY 0x540e
#define LXP_TIOCGPGRP 0x540f
#define LXP_TIOCSPGRP 0x5410
#define LXP_TIOCNOTTY 0x5422
/* c_lflag/c_iflag/c_oflag/c_cflag bits used for the canonical-tty default. */
#define LXP_ISIG 0x0001u
#define LXP_ICANON 0x0002u
#define LXP_ECHO 0x0008u
#define LXP_ICRNL 0x0100u
#define LXP_OPOST 0x0001u
#define LXP_ONLCR 0x0004u
#define LXP_CS8 0x0030u
#define LXP_CREAD 0x0080u
/* Signals: a per-process disposition table + the handful the shell cares about.
 * SIG_DFL/SIG_IGN are the special handler sentinels. Must cover the POSIX RT signals
 * (SIGRTMIN..): LinuxThreads uses __SIGRTMIN (>=32) for its thread restart/cancel/debug
 * signals, so a 32-wide table would reject the restart kill() and hang every pthread. */
#define LXP_NSIG 65
#define LXP_SIG_DFL 0
#define LXP_SIG_IGN 1
#define LXP_SIGINT 2
#define LXP_SIGQUIT 3
#define LXP_SIGABRT 6
#define LXP_SIGKILL 9
#define LXP_SIGSEGV 11
#define LXP_SIGPIPE 13
#define LXP_SIGALRM 14
#define LXP_SIGTERM 15
#define LXP_SIGCHLD 17 /* child stop/exit; default action = IGNORE (never terminates) */
#define LXP_ITIMER_REAL 0 /* setitimer(): real-time countdown -> SIGALRM */
/* fcntl commands: F_DUPFD duplicates an fd (the shell dups stdin for its
 * interactive fd); the rest are benign get/set probes. */
#define LXP_F_DUPFD 0
#define LXP_F_GETFD 1
#define LXP_F_SETFD 2
#define LXP_F_GETFL 3
#define LXP_F_SETFL 4
#define LXP_F_DUPFD_CLOEXEC 1030
/* c_cc indices (Linux generic, NCCS=19). */
#define LXP_VINTR 0
#define LXP_VERASE 2
#define LXP_VEOF 4
#define LXP_VMIN 6
#define LXP_NCCS 19
/* statx: AT_EMPTY_PATH means "stat the dirfd itself" (fstat); the basic-stats
 * result mask reported back in stx_mask. */
#define LXP_AT_EMPTY_PATH 0x1000
#define LXP_AT_SYMLINK_NOFOLLOW 0x100
#define LXP_STATX_BASIC_STATS 0x000007ffu

/* Linux errno values returned (negated) on syscall failure. */
#define LXP_ENOENT 2
#define LXP_ESRCH 3
#define LXP_EINTR 4
#define LXP_EIO 5
#define LXP_ENOEXEC 8
#define LXP_EBADF 9
#define LXP_ECHILD 10
#define LXP_EAGAIN 11
#define LXP_ENOMEM 12
#define LXP_EACCES 13
#define LXP_EFAULT 14
#define LXP_ENODEV 19
#define LXP_ENOTDIR 20
#define LXP_EISDIR 21
#define LXP_EMFILE 24
#define LXP_ENOTTY 25
#define LXP_EFBIG 27
#define LXP_ESPIPE 29
#define LXP_EROFS 30
#define LXP_EPIPE 32
#define LXP_EEXIST 17
#define LXP_EINVAL 22
#define LXP_ENOSPC 28
#define LXP_ERANGE 34
#define LXP_ENAMETOOLONG 36
#define LXP_ENOTEMPTY 39
#define LXP_ENOSYS 38
/* socket errnos (asm-generic values; ARM shares them). */
#define LXP_ENOTSOCK 88
#define LXP_EMSGSIZE 90
#define LXP_EPROTONOSUPPORT 93
#define LXP_EOPNOTSUPP 95
#define LXP_EAFNOSUPPORT 97
#define LXP_EADDRINUSE 98
#define LXP_ENETUNREACH 101
#define LXP_ECONNRESET 104
#define LXP_EISCONN 106
#define LXP_ENOTCONN 107
#define LXP_ECONNREFUSED 111
#define LXP_EHOSTUNREACH 113
#define LXP_EALREADY 114
#define LXP_EINPROGRESS 115
#define LXP_ESTALE 116 /* a remote-fs fid invalidated by a server reconnect */

/** Scatter/gather element, matching the target's @c struct iovec layout. */
typedef struct lxp_iovec {
	void *iov_base; /**< Start of the buffer (in the program's address space). */
	size_t iov_len; /**< Length of the buffer in bytes. */
} lxp_iovec;

/** Kernel @c struct termios (ARM, NCCS=19), filled by the TCGETS ioctl. */
typedef struct lxp_termios {
	uint32_t c_iflag;
	uint32_t c_oflag;
	uint32_t c_cflag;
	uint32_t c_lflag;
	uint8_t c_line;
	uint8_t c_cc[LXP_NCCS];
} lxp_termios;

/** @c struct winsize returned by TIOCGWINSZ. */
typedef struct lxp_winsize {
	uint16_t ws_row;
	uint16_t ws_col;
	uint16_t ws_xpixel;
	uint16_t ws_ypixel;
} lxp_winsize;

/** @c struct pollfd for poll(2). */
typedef struct lxp_pollfd {
	int fd;
	short events;
	short revents;
} lxp_pollfd;
#define LXP_POLLIN 0x0001
#define LXP_POLLOUT 0x0004

/** fd 1/2 output sink. Returns bytes written or a negated Linux errno. */
typedef long (*lxp_write_fn)(void *ctx, int fd, const void *buf, size_t len);
/** fd 0 input source. Returns bytes read (0 = EOF) or a negated Linux errno. */
typedef long (*lxp_read_fn)(void *ctx, int fd, void *buf, size_t len);

/** One node in the read-only in-memory rootfs (a flat path → bytes table). */
typedef struct lxp_file {
	const char *path;    /**< Absolute path, e.g. "/etc/hostname". */
	const uint8_t *data; /**< File contents (NULL for a directory). */
	size_t size;	     /**< Length in bytes (0 for a directory). */
	uint32_t mode;	     /**< st_mode; 0 means a regular file. Set @c LXP_S_IFDIR
			      *   for directories (their children are the entries one
			      *   path component below @c path). */
} lxp_file_t;

/** Open-file-descriptor slot. */
typedef struct lxp_fd {
	uint8_t kind;  /**< 0 = free, 1 = console, 2 = rootfs file, 3 = pipe, 4 = tmpfs,
			*   5 = /proc, 6 = device (values 4-6 are private to the syscall +
			*   device layers; only FD_DEV is exported below). */
	uint8_t rw;    /**< pipe end: 0 = read, 1 = write (kind == pipe). */
	uint8_t cloexec; /**< FD_CLOEXEC / O_CLOEXEC: closed on execve (dropbear's exec-status
			  *   pipe relies on this to detect a successful exec of the shell). */
	uint8_t nonblock; /**< O_NONBLOCK: a pipe read/write returns -EAGAIN instead of parking
			   *   (dropbear's SIGCHLD self-pipe is drained with a non-blocking read
			   *   loop; without this the final empty read parks forever). */
	int file_idx;  /**< rootfs index (file) / pipe index (pipe) / open-pool index (device). */
	size_t offset; /**< Read cursor (kind == file). */
} lxp_fd_t;

/** Device fd kind (shared by the syscall + device layers; the FD_FREE..FD_PROC
 *  kinds stay private to ove_linux_syscall.c). @c file_idx = open-pool index. */
#define LXP_FD_DEV 6
/** Socket fd kind (shared by the syscall + socket layers). @c file_idx = open-pool index. */
#define LXP_FD_SOCKET 7
/** eventfd fd kind (thread wakeup counter). @c file_idx = eventfd-pool index. */
#define LXP_FD_EVENTFD 8
/** pty fd kind (pseudo-terminal). @c file_idx = pty-pool index; @c rw = 1 master, 0 slave. */
#define LXP_FD_PTY 9
/** remote-fs fd kind (9P mount, e.g. /mnt/pi). @c file_idx = netfs open-pool index. */
#define LXP_FD_NET 10
/* eventfd2(2) flags. */
#define LXP_EFD_SEMAPHORE 0x00000001
#define LXP_EFD_NONBLOCK 0x00000800
#define LXP_EFD_CLOEXEC 0x00080000

/** Maximum simultaneously-open file descriptors per process. A fork-per-connection
 * server (httpd) holds std streams + the listener + the accepted client, per proc. */
#define LXP_MAX_FDS 32
/** Maximum path length (absolute, normalized) the personality resolves. */
#define LXP_PATH_MAX 256
/** Max exited children queued for wait4 (a pipeline forks several). */
#define LXP_MAX_CHILD 8
/** Bounds for an execve() argument vector captured for the engine to relaunch. */
#define LXP_EXEC_MAXARGS 8
#define LXP_EXEC_ARGBUF 256

/**
 * @brief A Linux process context — the state syscalls act on.
 *
 * NOMMU model: a bounded program break + anonymous mmap carved from an
 * @c ove_arena, a small fd table over standard streams (caller callbacks) and a
 * read-only in-memory rootfs, and an exit latch. Signals / a writable VFS /
 * fork+exec land in later phases.
 */
typedef struct lxp_proc {
	lxp_arena_t *arena;		/**< Backs @c brk and anonymous @c mmap. */
	uintptr_t brk_base;		/**< Initial program break. */
	uintptr_t brk_cur;		/**< Current program break. */
	uintptr_t brk_max;		/**< Ceiling imposed by the arena reservation. */
	lxp_write_fn write_fn;	/**< fd 1/2 sink; NULL → @c -LXP_EBADF. */
	lxp_read_fn read_fn;	/**< fd 0 source; NULL → EOF. */
	int (*console_poll)(void *ctx); /**< Optional non-blocking "key available?" for poll(2). */
	void *io_ctx;			/**< Opaque, passed to @c write_fn / @c read_fn. */
	const lxp_file_t *fs;	/**< Read-only rootfs table (NULL → no files). */
	int fs_count;			/**< Number of entries in @c fs. */
	lxp_fd_t fds[LXP_MAX_FDS]; /**< fd table; 0/1/2 are the std streams. */
	int pid;			   /**< This process's id (1 for the initial program). */
	int ppid;			   /**< Parent process id (0 for the initial program). */
	char comm[16];			   /**< Program name (argv[0] basename) for ps/top. */
	char cwd[LXP_PATH_MAX];	   /**< Current working directory (absolute, normalized). */
	int exited;			   /**< Set once @c exit / @c exit_group is called. */
	int exit_status;		   /**< Low 8 bits of the exit code. */
	/* Queue of exited (zombie) children awaiting wait4, FIFO. A pipeline forks
	 * more than one child, so a single slot is not enough. */
	int child_pid[LXP_MAX_CHILD];    /**< pids of exited children. */
	int child_status[LXP_MAX_CHILD]; /**< their exit codes. */
	int child_count;		     /**< number queued. */
	/* Signal disposition: per-signal handler address (or SIG_DFL/SIG_IGN) and
	 * the libc-supplied sa_restorer the engine returns to after the handler. */
	uintptr_t sig_handler[LXP_NSIG];
	uintptr_t sig_restorer[LXP_NSIG];
	/* execve request: the engine seam relaunches the thread on this rootfs
	 * program with the captured argument vector (image replacement). */
	int exec_pending;			 /**< Set when execve() should relaunch. */
	int exec_file_idx;			 /**< Rootfs index of the program to run. */
	int exec_argc;				 /**< Captured argument count. */
	char *exec_argv[LXP_EXEC_MAXARGS];	 /**< Captured argv (into exec_argv_buf). */
	char exec_argv_buf[LXP_EXEC_ARGBUF]; /**< Backing store for exec_argv. */
	/* nanosleep request: the dispatch parks the program and the run loop delays
	 * to the deadline (so RTOS idle/kernel threads run + time advances). */
	int sleep_pending;	 /**< Set when nanosleep() should park + delay. */
	uint64_t sleep_until_us; /**< Absolute wake deadline (ove_time_get_us base). */
	/* Concurrent process model (Phase D): the run loop is a coordinator over the
	 * live process SET, not a stack. These were the run-loop locals top/R[]/rowner[]/
	 * vctx[]; the per-slot resume contexts live in lxp_run.c. */
	int alive;	  /**< This slot holds a live process. */
	int region;	  /**< Program-image region index this proc runs in. */
	int region_owner; /**< 1 = owns/must-free its region; 0 = shares a parent's (vfork window). */
	/* access_ok validation ranges — the program's OWN writable memory. region_lo/hi = its image
	 * region [base, base+512K); pool_lo/hi = its dynamic-link arena (== region for a static proc).
	 * A syscall rejects (-EFAULT) any user pointer+len not wholly inside these (a READ source may
	 * also point into the shared read-only rootfs). Filled at launch; a vfork child / thread inherits
	 * its parent's. Guards the confused-deputy vector (the syscall handlers run PRIVILEGED). */
	uintptr_t region_lo, region_hi, pool_lo, pool_hi;
	int vfork_parent_slot; /**< Slot of a parent suspended awaiting this child's exec/exit, or -1. */
	/* vfork data isolation (NOMMU has no copy-on-write): a vfork child SHARES the parent's region,
	 * so its pre-exec writes (e.g. a libc signal-disposition reset) would corrupt the suspended
	 * parent. The coordinator snapshots the parent's writable data into a spare region at EV_FORK
	 * and restores it before the parent resumes (EV_EXEC/EV_EXIT). See vfork_snapshot/vfork_restore. */
	int snap_region;    /**< Scratch region index holding the parent's data snapshot, or -1 (none). */
	uintptr_t stack_lo; /**< Boundary between this proc's in-region writable data and its stack. */
	int is_dynamic;	    /**< FDPIC dynamic exec: its arena (libc RW data + heap) lives in the dyn_pool. */
	int fork_pending; /**< This proc issued vfork/fork/clone; coordinator spawns a child. */
	int is_thread;	  /**< This proc is a pthread: shares its creator's region for life. */
	int is_fdpic;	  /**< Program is FDPIC: signal handlers/restorers are funcdescs {entry,GOT}. */
	unsigned short umask; /**< umask(2) file-creation mask; 022 at launch, inherited on fork. */
	int clone_is_thread;	     /**< Pending fork is a clone(CLONE_VM) thread (set at the
				      *   syscall, consumed by the coordinator's EV_FORK). */
	uintptr_t clone_child_stack; /**< clone(2) child_stack arg: the new thread runs on this. */
	int sigsuspend_pending;	     /**< Parked in rt_sigsuspend; woken by a delivered signal (the
				      *   LinuxThreads restart) — the coordinator runs the handler. */
	int sleeping;	  /**< Parked for nanosleep until sleep_until_us. */
	int wait_pending; /**< Blocked in wait4 (parked) awaiting a child exit. */
	int wait_pid;	  /**< wait4 pid arg (-1 = any child). */
	int wait_nohang;  /**< WNOHANG was set. */
	uintptr_t wait_status_p; /**< User int* to fill with the wait status on wake. */
	int live_children;	 /**< Count of live (un-reaped) children, for wait4. */
	/* Blocking pipe I/O (Phase D2): a read on an empty pipe with a writer still open,
	 * or a write on a full pipe with a reader still open, parks the proc; the
	 * coordinator retries each pass and resumes it when the peer drains/fills. */
	int pipe_wait;	    /**< 0 = none, 1 = blocked reading, 2 = blocked writing. */
	int pipe_idx;	    /**< g_pipes[] index being waited on. */
	uintptr_t pipe_buf; /**< User buffer for the parked read/write. */
	size_t pipe_len;    /**< Requested length for the parked read/write. */
	int console_wait;      /**< 1 = blocked reading the console; the coordinator polls it. */
	uintptr_t console_buf; /**< User buffer for the parked console read. */
	size_t console_len;    /**< Requested length for the parked console read. */
	/* Cross-process signal (Phase D3): kill(pid,sig) from another proc latches the
	 * signal here; it is delivered at this proc's next syscall boundary (if running)
	 * or by the coordinator (if parked in sleep/wait/pipe). 0 = none. */
	int pending_sig;
	/* Blocking device I/O (P0 device layer): a read/write/ioctl on a /dev node that
	 * would block parks the proc; the coordinator retries via lxp_dev_retry (the
	 * same park/retry as pipe_wait) and resumes it on completion. */
	uint8_t dev_wait;	  /**< 0 = none, else a DEVW_* op the coordinator retries. */
	int dev_oi;		  /**< Open-pool index being waited on. */
	uintptr_t dev_buf;	  /**< User buffer (read/write) or ioctl arg. */
	size_t dev_len;		  /**< Requested length (read/write). */
	unsigned long dev_cmd;	  /**< ioctl command. */
	uint64_t dev_deadline_us; /**< 0 = infinite (poll timeout / read VTIME). */
	/* Device mmap ranges (P3): a successful /dev mmap records its [lo,hi) here so
	 * user_ok accepts the mapped framebuffer (two mappings max). */
	uintptr_t dev_map_lo[2], dev_map_hi[2];
	/* Blocking socket I/O (P0 socket layer): a connect/send/recv on a socket that
	 * would block parks the proc; the coordinator retries via lxp_sock_retry
	 * (the same park/retry as dev_wait) and resumes it on completion. */
	uint8_t sock_wait;  /**< 0 = none, else a SOCKW_* op the coordinator retries. */
	int sock_oi;	    /**< Socket open-pool index being waited on. */
	uintptr_t sock_buf; /**< User buffer (send/recv); the user pollfd array for SOCKW_POLL. */
	size_t sock_len;    /**< Requested length (send/recv); nfds for SOCKW_POLL. */
	uint64_t sock_deadline_us; /**< SOCKW_POLL absolute timeout (UINT64_MAX = infinite). */
	/* pselect6(2): a parked select reuses the SOCKW_POLL machinery but re-derives the
	 * poll set from the caller's fd_sets each retry (they are untouched until it
	 * completes, then written in place). Non-zero sel_active flags this to the retry. */
	uint8_t sel_active;   /**< 1 = the parked poll originated from pselect6. */
	int sel_nfds;	      /**< pselect nfds (highest fd + 1). */
	uintptr_t sel_rfds;   /**< user readfds fd_set* (0 = NULL). */
	uintptr_t sel_wfds;   /**< user writefds fd_set* (0 = NULL). */
	uintptr_t sel_efds;   /**< user exceptfds fd_set* (0 = NULL). */
	/* Blocking pty I/O (pseudo-terminal layer): a read on an empty ring or a write on a
	 * full ring parks the proc; the coordinator retries via lxp_pty_retry and resumes
	 * it when the peer end drains/fills (same park/retry as pipe_wait). */
	uint8_t pty_wait;   /**< 0 = none, else a PTYW_* op the coordinator retries. */
	int pty_idx;	    /**< g_ptys[] index being waited on. */
	uintptr_t pty_buf;  /**< User buffer for the parked read/write. */
	size_t pty_len;	    /**< Requested length for the parked read/write. */
	uint64_t alarm_deadline_us; /**< setitimer(ITIMER_REAL)/alarm() fire time; 0 = disarmed. */
	uint64_t alarm_interval_us; /**< Repeating interval (0 = one-shot); re-arms on fire. */
		/* Blocking remote-fs I/O (9P netfs layer, /mnt/pi): an open/read/getdents/stat that
		 * needs a Pi round-trip parks the proc; the coordinator pumps the 9P transport each
		 * pass via lxp_netfs_retry and resumes it on completion (same park/retry as
		 * dev_wait/sock_wait). The heavy request state lives in the netfs request pool. */
		uint8_t netfs_wait; /**< 0 = none, else a NETFSW_* op the coordinator retries. */
		int netfs_oi;	    /**< netfs open-pool index being waited on (-1 for a path-only op). */
		int netfs_req;	    /**< netfs request-pool index driving this parked op. */
		uintptr_t netfs_buf; /**< User buffer (read) / stat-out / getdents buffer. */
		size_t netfs_len;    /**< Requested length / buffer capacity. */
		uint64_t netfs_deadline_us; /**< Absolute-µs per-request timeout (UINT64_MAX = infinite). */
} lxp_proc_t;

/** @brief Proc-table accessors (defined in the run loop) so the pipe layer can scan
 * all live procs' fds to count a pipe's open read/write ends (for EOF / EPIPE). */
lxp_proc_t *lxp_proc_table(void);
int lxp_proc_nslot(void);

/** @brief Install a kernel object (@p kind, @p idx) into @p p's fd table, returning the
 * lowest free fd or @c -LXP_EMFILE. Lets the socket bridge mint an accept(2) fd. */
int lxp_fd_install(lxp_proc_t *p, uint8_t kind, int idx);

/** @brief Retry a parked pipe read/write (run-loop coordinator). Returns the byte
 * count, 0 (EOF), or -EPIPE on completion; @c -LXP_EAGAIN while still blocked. */
long lxp_pipe_retry(lxp_proc_t *p);

/**
 * @brief Attach a read-only in-memory rootfs the program can @c open / @c read.
 * @note Requires @c LXP_ENABLE_LINUX.
 */
void lxp_proc_set_rootfs(lxp_proc_t *proc, const lxp_file_t *files, int count);

/**
 * @brief Parse a newc CPIO archive (e.g. a Buildroot rootfs.cpio) into a rootfs
 *        table usable by @ref lxp_proc_set_rootfs.
 *
 * Each entry's relative name gets a leading "/" written into @p namebuf (a
 * leading "./" is stripped); regular-file @c data points into @p cpio in place.
 * Stops at the "TRAILER!!!" entry.
 *
 * @return number of entries, or -1 on malformed input / table-or-namebuf overflow.
 * @note Requires @c LXP_ENABLE_LINUX.
 */
int lxp_cpio_to_rootfs(const uint8_t *cpio, size_t len, lxp_file_t *out, int max_entries,
			   char *namebuf, size_t namebuf_len);

/**
 * @brief Declare a memory-mapped rootfs image window so the coordinator task reads it safely.
 *
 * Call once, from the coordinator task, BEFORE the first read of a rootfs image that lives in a
 * memory-mapped device window (before @ref lxp_cpio_to_rootfs and any @ref lxp_run over
 * it).  On most engines/boards this is a no-op.  On the STM32F746 with the QUADSPI-XIP rootfs and
 * the M7 D-cache enabled, the FreeRTOS backend installs a bounded, non-cacheable per-task MPU
 * region over [base, base+len) so the cache never issues a line-fill burst — nor speculatively
 * prefetches past the flash chip — into the memory-mapped NOR (both corrupt the read otherwise).
 *
 * @param base start of the memory-mapped rootfs window.
 * @param len  size of the window in bytes (an upper bound is fine; use the mapped device size).
 */
void lxp_rootfs_window(const void *base, size_t len);

/**
 * @brief Make a guest buffer coherent before the coordinator reads it for the transport.
 *
 * Call from the coordinator, on a guest-supplied buffer, immediately BEFORE handing it to an
 * engine transport that will read it from physical memory (e.g. @ref ove_socket_send, whose
 * lwIP copy runs in the privileged coordinator context).  On most engines/boards this is a
 * no-op.  On the STM32F746 with the M7 D-cache enabled, the guest writes this SDRAM buffer
 * through its Normal-cacheable MPU region, so the freshly written bytes can still sit in dirty
 * D-cache lines while physical SDRAM holds stale data; the coordinator reads the SAME SDRAM
 * through its uncached (Device) background view and would copy the stale bytes.  The FreeRTOS
 * backend cleans (writes back) the buffer's D-cache lines so both views agree.  The tail of a
 * just-built buffer is the most-recently-written and thus the most likely victim.
 *
 * @param base start of the guest buffer the transport is about to read.
 * @param len  number of bytes the transport will read.
 */
void lxp_guest_flush(const void *base, size_t len);

/**
 * @brief Invalidate the guest's D-cache over [base, len) so its next read refills from SDRAM.
 *
 * The inverse of @ref lxp_guest_flush: after the coordinator has WRITTEN guest memory through
 * its uncached view (the vfork data-isolation restore), the guest's cached lines are stale and must
 * be discarded (invalidate, not clean — a clean would overwrite the coordinator's fresh SDRAM).
 */
void lxp_guest_invalidate(const void *base, size_t len);

/* ---- OS-service hooks routed through the engine ops ------------------------
 * The personality core calls these module-internal wrappers instead of the
 * host's clock / cache primitives; the per-engine seam fills the underlying
 * ops (see struct lxp_engine). This lets the personality build against any
 * host without referencing ove_time_* directly.
 *
 * lxp_cache_clean/invalidate are the ops-routed equivalents of
 * lxp_guest_flush/invalidate above (which remain for the direct callers
 * still using the weak-symbol form during the extraction). */
int lxp_time_us(uint64_t *out);
int lxp_time_ns(uint64_t *out);
void lxp_cache_clean(const void *base, size_t len);
void lxp_cache_invalidate(const void *base, size_t len);
struct lxp_thread_info;
int lxp_thread_list(struct lxp_thread_info *out, size_t max_count, size_t *actual_count);


/**
 * @brief Resolve an absolute path through a rootfs (following symlinks) to a file's bytes.
 *
 * Used by the run loop to locate the FDPIC interpreter (ld.so) at launch, before a proc
 * exists. Follows up to 8 symlink hops (each normalized against the link's directory).
 * @return 0 with @p data / @p len set, or a negative errno (@c -ENOENT if unresolved).
 */
long lxp_rootfs_resolve(const lxp_file_t *fs, int count, const char *abspath,
			    const uint8_t **data, size_t *len);

/**
 * @brief Initialise a process context with an arena-backed program break.
 *
 * Reserves @p brk_bytes from @p arena for the program break. The caller wires
 * @c write_fn / @c read_fn / @c io_ctx afterwards.
 *
 * @return LXP_OK; LXP_ERR_INVALID_PARAM on bad arguments;
 *         LXP_ERR_NO_MEMORY if the arena cannot satisfy @p brk_bytes.
 * @note Requires @c LXP_ENABLE_LINUX.
 */
int lxp_proc_init(lxp_proc_t *proc, lxp_arena_t *arena, size_t brk_bytes);

/* ELF auxiliary-vector types in the startup block (uClibc scans them after envp). */
#define LXP_AT_NULL 0
#define LXP_AT_PHDR 3  /* program headers (FDPIC crt finds PT_TLS etc. here) */
#define LXP_AT_PHENT 4 /* size of one program header (Elf32_Phdr = 32) */
#define LXP_AT_PHNUM 5 /* number of program headers */
#define LXP_AT_PAGESZ 6
#define LXP_AT_BASE 7 /* interpreter base (0: static, no ld.so) */
#define LXP_AT_ENTRY 9
#define LXP_AT_RANDOM 25

/**
 * @brief Build a uClinux/FDPIC process stack for a loaded program's crt0.
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
 * @param[in] fdpic      Non-zero → the standard ELF inline stack (FDPIC programs);
 *                       zero → the legacy uClinux 3-word argc/argv-ptr/envp-ptr header.
 * @param[in] phdr       FDPIC only: runtime program-header address (AT_PHDR).
 * @param[in] phnum      FDPIC only: number of program headers (AT_PHNUM).
 * @param[in] entry      FDPIC only: program entry point (AT_ENTRY).
 * @return The initial stack pointer, or NULL on bad arguments / insufficient room.
 * @note Requires @c LXP_ENABLE_LINUX.
 */
void *lxp_setup_stack(void *stack, size_t stack_size, int argc, const char *const argv[],
			  const char *const envp[], int fdpic, uintptr_t phdr, int phnum,
			  uintptr_t entry, uintptr_t at_base);

/**
 * @brief Dispatch one Linux syscall against @p proc.
 *
 * @param[in] proc Process context.
 * @param[in] nr   Linux syscall number (@c LXP_NR_*).
 * @param[in] a0..a5 Syscall arguments (register values; pointers are program
 *                   addresses).
 * @return The syscall result, Linux-ABI style: a non-negative value on success
 *         or a negated errno on failure. Unknown numbers return
 *         @c -LXP_ENOSYS.
 * @note Requires @c LXP_ENABLE_LINUX.
 */
long lxp_syscall(lxp_proc_t *proc, long nr, long a0, long a1, long a2, long a3, long a4,
		     long a5);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_LINUX_SYSCALL_H */
