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

/* fd-slot kinds (ove_lnx_fd.kind). */
#define OVE_LNX_FD_FREE 0
#define OVE_LNX_FD_CONSOLE 1
#define OVE_LNX_FD_FILE 2
#define OVE_LNX_FD_PIPE 3

/*
 * Pipe objects. A pipe is shared kernel state (like a real kernel's pipe inode):
 * the writer process fills the buffer, the reader process drains it. The model
 * is sequentialised, so the writer always runs to completion before the reader,
 * and "buffer empty" == EOF (no concurrent writer to wait for). Bounded buffer.
 */
#define OVE_LNX_NPIPE 4
#define OVE_LNX_PIPE_BUF 1024
typedef struct {
	uint8_t buf[OVE_LNX_PIPE_BUF];
	size_t rpos; /* bytes consumed by the reader */
	size_t wpos; /* bytes produced by the writer */
	int used;
} ove_lnx_pipe_t;
static ove_lnx_pipe_t g_pipes[OVE_LNX_NPIPE];

/*
 * Writable in-memory tmpfs overlaid on the read-only CPIO rootfs: open(O_CREAT)
 * creates a file here (e.g. shell redirects `echo x > /tmp/f`). Files persist
 * across processes (global kernel state, like pipes). Bounded; not freed.
 */
#define OVE_LNX_FD_TMPFS 4
#define OVE_LNX_NTMP 4
#define OVE_LNX_TMP_BUF 1024
typedef struct {
	char path[64];
	uint8_t buf[OVE_LNX_TMP_BUF];
	size_t size;
	int used;
} ove_lnx_tmpfile_t;
static ove_lnx_tmpfile_t g_tmp[OVE_LNX_NTMP];

static int tmp_find(const char *path)
{
	for (int i = 0; i < OVE_LNX_NTMP; i++)
		if (g_tmp[i].used && strcmp(g_tmp[i].path, path) == 0)
			return i;
	return -1;
}

/*
 * ARM kernel struct stat64. Spelled with fixed-width types (the kernel's
 * `unsigned long` is 32-bit on ARM but 64-bit on the x86-64 host) so the binary
 * layout is identical on target and in host tests.
 */
struct ove_lnx_kstat64 {
	uint64_t st_dev;
	uint8_t __pad0[4];
	uint32_t __st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint8_t __pad3[4];
	int64_t st_size;
	uint32_t st_blksize;
	uint64_t st_blocks;
	uint32_t st_atime;
	uint32_t st_atime_nsec;
	uint32_t st_mtime;
	uint32_t st_mtime_nsec;
	uint32_t st_ctime;
	uint32_t st_ctime_nsec;
	uint64_t st_ino;
};

/* getdents64 record: fixed 19-byte head (d_ino..d_type) then a NUL-terminated name. */
struct ove_lnx_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

/* Effective st_mode for a rootfs node (0 in the table means a regular file). */
static uint32_t file_mode(const ove_lnx_file_t *f)
{
	return f->mode ? f->mode : (OVE_LNX_S_IFREG | 0644u);
}

/* If @p path names an entry exactly one component below directory @p dir, return
 * that child's name; otherwise NULL. */
static const char *child_name(const char *dir, const char *path)
{
	if (dir[0] == '/' && dir[1] == 0) { /* root */
		if (path[0] != '/' || path[1] == 0)
			return NULL;
		return strchr(path + 1, '/') ? NULL : path + 1;
	}
	size_t dl = strlen(dir);
	if (strncmp(path, dir, dl) != 0 || path[dl] != '/')
		return NULL;
	const char *name = path + dl + 1;
	return (*name && !strchr(name, '/')) ? name : NULL;
}

int ove_lnx_proc_init(ove_lnx_proc_t *proc, ove_arena_t *arena, size_t brk_bytes)
{
	if (!proc || !arena)
		return OVE_ERR_INVALID_PARAM;

	memset(proc, 0, sizeof(*proc));
	proc->arena = arena;
	proc->pid = 1; /* the initial program is pid 1 (ppid 0); fork assigns the rest */
	/* fd 0/1/2 are the standard streams, routed to the caller's callbacks.
	 * For console fds, file_idx marks the direction: 0 = readable (stdin),
	 * 1 = writable (stdout/stderr); this survives F_DUPFD so a dup of stdin
	 * stays readable (the shell dups stdin for its interactive fd). */
	proc->fds[0].kind = OVE_LNX_FD_CONSOLE;
	proc->fds[0].file_idx = 0;
	proc->fds[1].kind = OVE_LNX_FD_CONSOLE;
	proc->fds[1].file_idx = 1;
	proc->fds[2].kind = OVE_LNX_FD_CONSOLE;
	proc->fds[2].file_idx = 1;
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

void ove_lnx_proc_set_rootfs(ove_lnx_proc_t *proc, const ove_lnx_file_t *files, int count)
{
	if (!proc)
		return;
	proc->fs = files;
	proc->fs_count = (files && count > 0) ? count : 0;
}

/* Parse 8 ASCII-hex chars (a newc CPIO header field). */
static uint32_t cpio_hex(const char *s)
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) {
		char c = s[i];
		uint32_t d = (c >= '0' && c <= '9')   ? (uint32_t)(c - '0')
			     : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
			     : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10)
						      : 0u;
		v = (v << 4) | d;
	}
	return v;
}

int ove_lnx_cpio_to_rootfs(const uint8_t *cpio, size_t len, ove_lnx_file_t *out, int max,
			   char *namebuf, size_t nblen)
{
	if (!cpio || !out || !namebuf)
		return -1;
	size_t pos = 0, nb = 0;
	int n = 0;
	while (pos + 110 <= len) {
		const char *h = (const char *)(cpio + pos);
		if (memcmp(h, "070701", 6) != 0) /* newc magic */
			return -1;
		uint32_t mode = cpio_hex(h + 14);  /* c_mode */
		uint32_t fsize = cpio_hex(h + 54); /* c_filesize */
		uint32_t nsize = cpio_hex(h + 94); /* c_namesize (incl NUL) */
		if (pos + 110 + nsize > len)
			return -1;
		const char *name = h + 110;
		if (strcmp(name, "TRAILER!!!") == 0)
			break;
		size_t data_off = (pos + 110 + nsize + 3u) & ~(size_t)3u;
		if (fsize && data_off + fsize > len)
			return -1;
		if (n >= max)
			return -1;
		/* Write "/" + relative-name (strip a leading "./") into namebuf. */
		const char *nm = name;
		if (nm[0] == '.' && nm[1] == '/')
			nm += 2;
		else if (nm[0] == '.' && nm[1] == 0)
			nm += 1; /* "." -> "" -> "/" */
		size_t l = strlen(nm);
		if (nb + 2 + l > nblen)
			return -1;
		char *path = namebuf + nb;
		path[0] = '/';
		memcpy(path + 1, nm, l + 1);
		nb += 2 + l;
		out[n].path = path;
		/* Keep content for regular files AND symlinks (the link target string),
		 * so exec can resolve /bin/<applet> -> busybox. Dirs have no content. */
		out[n].data = fsize ? (cpio + data_off) : NULL;
		out[n].size = fsize;
		out[n].mode = mode;
		n++;
		pos = (data_off + fsize + 3u) & ~(size_t)3u;
	}
	return n;
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

/* Validate an fd index and return its slot, or NULL. */
static ove_lnx_fd_t *fd_slot(ove_lnx_proc_t *p, int fd)
{
	if (fd < 0 || fd >= OVE_LNX_MAX_FDS || p->fds[fd].kind == OVE_LNX_FD_FREE)
		return NULL;
	return &p->fds[fd];
}

static long sys_write(ove_lnx_proc_t *p, int fd, const void *buf, size_t len)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (len && !buf)
		return -OVE_LNX_EFAULT;
	/* A pipe write end appends to the (bounded) shared pipe buffer. */
	if (s->kind == OVE_LNX_FD_PIPE) {
		if (s->rw != 1)
			return -OVE_LNX_EBADF;
		ove_lnx_pipe_t *pp = &g_pipes[s->file_idx];
		size_t space = OVE_LNX_PIPE_BUF - pp->wpos;
		if (len > space)
			len = space;
		memcpy(pp->buf + pp->wpos, buf, len);
		pp->wpos += len;
		return (long)len;
	}
	/* A tmpfs file write copies into its (bounded) buffer at the fd offset. */
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_tmpfile_t *t = &g_tmp[s->file_idx];
		if (s->offset >= OVE_LNX_TMP_BUF)
			return -OVE_LNX_EFBIG;
		size_t space = OVE_LNX_TMP_BUF - s->offset;
		if (len > space)
			len = space;
		memcpy(t->buf + s->offset, buf, len);
		s->offset += len;
		if (s->offset > t->size)
			t->size = s->offset;
		return (long)len;
	}
	/* Only output consoles are writable (file_idx != 0); the rootfs is read-only. */
	if (s->kind != OVE_LNX_FD_CONSOLE || s->file_idx == 0 || !p->write_fn)
		return -OVE_LNX_EBADF;
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
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (len && !buf)
		return -OVE_LNX_EFAULT;

	if (s->kind == OVE_LNX_FD_CONSOLE) {
		if (s->file_idx != 0) /* output consoles (stdout/stderr) are not readable */
			return -OVE_LNX_EBADF;
		if (!p->read_fn)
			return 0; /* EOF */
		return p->read_fn(p->io_ctx, fd, buf, len);
	}

	/* A pipe read end drains the shared buffer; empty == EOF (sequentialised:
	 * the writer has already run to completion, so no more data will arrive). */
	if (s->kind == OVE_LNX_FD_PIPE) {
		if (s->rw != 0)
			return -OVE_LNX_EBADF;
		ove_lnx_pipe_t *pp = &g_pipes[s->file_idx];
		size_t avail = pp->wpos - pp->rpos;
		if (avail == 0)
			return 0; /* EOF */
		if (len > avail)
			len = avail;
		memcpy(buf, pp->buf + pp->rpos, len);
		pp->rpos += len;
		return (long)len;
	}

	/* A tmpfs file read returns bytes from its buffer at the fd offset. */
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_tmpfile_t *t = &g_tmp[s->file_idx];
		if (s->offset >= t->size)
			return 0; /* EOF */
		size_t n = t->size - s->offset;
		if (n > len)
			n = len;
		memcpy(buf, t->buf + s->offset, n);
		s->offset += n;
		return (long)n;
	}

	/* Read from a rootfs file at the current offset. */
	const ove_lnx_file_t *f = &p->fs[s->file_idx];
	if ((file_mode(f) & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
		return -OVE_LNX_EISDIR;
	if (s->offset >= f->size)
		return 0; /* EOF */
	size_t n = f->size - s->offset;
	if (n > len)
		n = len;
	memcpy(buf, f->data + s->offset, n);
	s->offset += n;
	return (long)n;
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
	(void)fd; /* anonymous mappings ignore fd; file mmap is unsupported */
	if (!(flags & OVE_LNX_MAP_ANONYMOUS))
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

/* open a rootfs file read-only; the fs is immutable, so writes are refused. */
/* Claim the lowest free fd for (kind, idx, off); -EMFILE if the table is full. */
static int fd_alloc(ove_lnx_proc_t *p, uint8_t kind, int idx, size_t off)
{
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++) {
		if (p->fds[fd].kind == OVE_LNX_FD_FREE) {
			p->fds[fd].kind = kind;
			p->fds[fd].rw = 0;
			p->fds[fd].file_idx = idx;
			p->fds[fd].offset = off;
			return fd;
		}
	}
	return -OVE_LNX_EMFILE;
}

static long sys_openat(ove_lnx_proc_t *p, int dirfd, const char *path, int flags)
{
	(void)dirfd; /* rootfs paths are absolute */
	if (!path)
		return -OVE_LNX_EFAULT;
	int wr = (flags & OVE_LNX_O_ACCMODE) != OVE_LNX_O_RDONLY;
	int ti = tmp_find(path);

	/* A writable open (or O_CREAT) goes to the writable tmpfs overlay. */
	if (wr || (flags & OVE_LNX_O_CREAT)) {
		if (ti < 0) {
			if (!(flags & OVE_LNX_O_CREAT)) {
				for (int i = 0; i < p->fs_count; i++)
					if (strcmp(p->fs[i].path, path) == 0)
						return -OVE_LNX_EROFS; /* RO rootfs file */
				return -OVE_LNX_ENOENT;
			}
			for (int i = 0; i < OVE_LNX_NTMP; i++)
				if (!g_tmp[i].used) {
					ti = i;
					break;
				}
			if (ti < 0)
				return -OVE_LNX_EMFILE;
			if (strlen(path) >= sizeof(g_tmp[ti].path))
				return -OVE_LNX_EINVAL;
			strcpy(g_tmp[ti].path, path);
			g_tmp[ti].size = 0;
			g_tmp[ti].used = 1;
		} else if (flags & OVE_LNX_O_TRUNC) {
			g_tmp[ti].size = 0;
		}
		return fd_alloc(p, OVE_LNX_FD_TMPFS, ti,
				(flags & OVE_LNX_O_APPEND) ? g_tmp[ti].size : 0);
	}

	/* Read: a created tmpfs file shadows the rootfs; else the read-only rootfs. */
	if (ti >= 0)
		return fd_alloc(p, OVE_LNX_FD_TMPFS, ti, 0);
	for (int i = 0; i < p->fs_count; i++)
		if (strcmp(p->fs[i].path, path) == 0)
			return fd_alloc(p, OVE_LNX_FD_FILE, i, 0);
	return -OVE_LNX_ENOENT;
}

static long sys_close(ove_lnx_proc_t *p, int fd)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	s->kind = OVE_LNX_FD_FREE;
	return 0;
}

/* pipe(2): allocate a pipe object + a read-end / write-end fd pair. */
static long sys_pipe(ove_lnx_proc_t *p, int *fds)
{
	if (!fds)
		return -OVE_LNX_EFAULT;
	int pi = -1;
	for (int i = 0; i < OVE_LNX_NPIPE; i++) {
		if (!g_pipes[i].used) {
			pi = i;
			break;
		}
	}
	if (pi < 0)
		return -OVE_LNX_EMFILE;
	int rfd = -1, wfd = -1;
	for (int fd = 0; fd < OVE_LNX_MAX_FDS && wfd < 0; fd++) {
		if (p->fds[fd].kind != OVE_LNX_FD_FREE)
			continue;
		if (rfd < 0)
			rfd = fd;
		else
			wfd = fd;
	}
	if (wfd < 0)
		return -OVE_LNX_EMFILE;
	g_pipes[pi].used = 1;
	g_pipes[pi].rpos = 0;
	g_pipes[pi].wpos = 0;
	p->fds[rfd] = (ove_lnx_fd_t){.kind = OVE_LNX_FD_PIPE, .rw = 0, .file_idx = pi};
	p->fds[wfd] = (ove_lnx_fd_t){.kind = OVE_LNX_FD_PIPE, .rw = 1, .file_idx = pi};
	fds[0] = rfd;
	fds[1] = wfd;
	return 0;
}

/* dup2/dup3: make newfd alias oldfd's target (the pipe wiring the shell does). */
static long sys_dup2(ove_lnx_proc_t *p, int oldfd, int newfd)
{
	ove_lnx_fd_t *s = fd_slot(p, oldfd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (newfd < 0 || newfd >= OVE_LNX_MAX_FDS)
		return -OVE_LNX_EBADF;
	if (oldfd != newfd)
		p->fds[newfd] = *s;
	return newfd;
}

/* dup(2): alias oldfd onto the lowest free fd. */
static long sys_dup(ove_lnx_proc_t *p, int oldfd)
{
	ove_lnx_fd_t *s = fd_slot(p, oldfd);
	if (!s)
		return -OVE_LNX_EBADF;
	for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++) {
		if (p->fds[fd].kind == OVE_LNX_FD_FREE) {
			p->fds[fd] = *s;
			return fd;
		}
	}
	return -OVE_LNX_EMFILE;
}

static long sys_lseek(ove_lnx_proc_t *p, int fd, long off, int whence)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (s->kind != OVE_LNX_FD_FILE && s->kind != OVE_LNX_FD_TMPFS)
		return -OVE_LNX_ESPIPE; /* console/pipe is not seekable */

	long end = (s->kind == OVE_LNX_FD_TMPFS) ? (long)g_tmp[s->file_idx].size
						 : (long)p->fs[s->file_idx].size;
	long base;
	switch (whence) {
	case OVE_LNX_SEEK_SET:
		base = 0;
		break;
	case OVE_LNX_SEEK_CUR:
		base = (long)s->offset;
		break;
	case OVE_LNX_SEEK_END:
		base = end;
		break;
	default:
		return -OVE_LNX_EINVAL;
	}
	long pos = base + off;
	if (pos < 0)
		return -OVE_LNX_EINVAL;
	s->offset = (size_t)pos;
	return pos;
}

static long sys_fstat64(ove_lnx_proc_t *p, int fd, void *statbuf)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (!statbuf)
		return -OVE_LNX_EFAULT;

	struct ove_lnx_kstat64 *st = statbuf;
	memset(st, 0, sizeof(*st));
	st->st_nlink = 1;
	if (s->kind == OVE_LNX_FD_FILE) {
		const ove_lnx_file_t *f = &p->fs[s->file_idx];
		st->st_mode = file_mode(f);
		st->st_size = (int64_t)f->size;
		st->st_blksize = 512;
		st->st_blocks = (uint64_t)((f->size + 511u) / 512u);
	} else if (s->kind == OVE_LNX_FD_TMPFS) {
		size_t sz = g_tmp[s->file_idx].size;
		st->st_mode = OVE_LNX_S_IFREG | 0644u;
		st->st_size = (int64_t)sz;
		st->st_blksize = 512;
		st->st_blocks = (uint64_t)((sz + 511u) / 512u);
	} else {
		/* A character device: makes uClibc block-buffer stdio. */
		st->st_mode = OVE_LNX_S_IFCHR | 0620u;
		st->st_blksize = 1024;
	}
	return 0;
}

/* getdents64: emit the directory's immediate children as linux_dirent64 records. */
static long sys_getdents64(ove_lnx_proc_t *p, int fd, void *buf, size_t count)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s || s->kind != OVE_LNX_FD_FILE)
		return -OVE_LNX_EBADF;
	if (!buf)
		return -OVE_LNX_EFAULT;
	const ove_lnx_file_t *dir = &p->fs[s->file_idx];
	if ((file_mode(dir) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFDIR)
		return -OVE_LNX_ENOTDIR;

	uint8_t *out = (uint8_t *)buf;
	size_t filled = 0;
	long pos = 0; /* running child index; s->offset = how many already emitted */
	for (int i = 0; i < p->fs_count; i++) {
		const char *name = child_name(dir->path, p->fs[i].path);
		if (!name)
			continue;
		if (pos < (long)s->offset) {
			pos++;
			continue;
		}
		size_t namelen = strlen(name);
		size_t reclen = (offsetof(struct ove_lnx_dirent64, d_name) + namelen + 1 + 7u) &
				~(size_t)7u;
		if (filled + reclen > count) {
			if (filled == 0)
				return -OVE_LNX_EINVAL; /* buffer too small for one entry */
			break;
		}
		struct ove_lnx_dirent64 *de = (struct ove_lnx_dirent64 *)(out + filled);
		de->d_ino = (uint64_t)(i + 1);
		de->d_off = pos + 1;
		de->d_reclen = (uint16_t)reclen;
		de->d_type = ((file_mode(&p->fs[i]) & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
				     ? OVE_LNX_DT_DIR
				     : OVE_LNX_DT_REG;
		memcpy(de->d_name, name, namelen + 1);
		filled += reclen;
		pos++;
		s->offset++;
	}
	return (long)filled;
}

/* Modern struct statx (256 bytes); fixed-width so host tests match the target. */
struct ove_lnx_statx {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __spare0;
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	uint8_t __rest[256 - 64];
};

/*
 * statx: the stat() uClibc-ng actually issues. With AT_EMPTY_PATH (or an empty
 * path) it stats the open dirfd (fstat); otherwise it resolves a rootfs path.
 */
static long sys_statx(ove_lnx_proc_t *p, int dirfd, const char *path, int flags, void *buf)
{
	if (!buf)
		return -OVE_LNX_EFAULT;

	uint32_t mode;
	uint64_t size;
	if (path && path[0] && !(flags & OVE_LNX_AT_EMPTY_PATH)) {
		int idx = -1;
		for (int i = 0; i < p->fs_count; i++) {
			if (strcmp(p->fs[i].path, path) == 0) {
				idx = i;
				break;
			}
		}
		if (idx < 0)
			return -OVE_LNX_ENOENT;
		mode = file_mode(&p->fs[idx]);
		size = p->fs[idx].size;
	} else {
		ove_lnx_fd_t *s = fd_slot(p, dirfd);
		if (!s)
			return -OVE_LNX_EBADF;
		if (s->kind == OVE_LNX_FD_FILE) {
			mode = file_mode(&p->fs[s->file_idx]);
			size = p->fs[s->file_idx].size;
		} else {
			mode = OVE_LNX_S_IFCHR | 0620u;
			size = 0;
		}
	}

	struct ove_lnx_statx *st = buf;
	memset(st, 0, sizeof(*st));
	st->stx_mask = OVE_LNX_STATX_BASIC_STATS;
	st->stx_blksize = 512;
	st->stx_nlink = 1;
	st->stx_mode = (uint16_t)mode;
	st->stx_size = size;
	st->stx_blocks = (size + 511u) / 512u;
	return 0;
}

/*
 * execve: resolve the program in the rootfs and capture its argument vector,
 * then flag the request. The per-engine seam (privileged) does the actual image
 * replacement — reload the bFLT, rebuild the MPU domain + stack, and relaunch
 * the thread — because that is engine-specific. We never truly return: on
 * success the old image is gone; on failure we report a negated errno.
 */
static long sys_execve(ove_lnx_proc_t *p, const char *path, char *const argv[])
{
	if (!path)
		return -OVE_LNX_EFAULT;
	int idx = -1;
	for (int i = 0; i < p->fs_count; i++) {
		if (strcmp(p->fs[i].path, path) == 0) {
			idx = i;
			break;
		}
	}
	if (idx < 0)
		return -OVE_LNX_ENOENT;
	/* Follow symlinks, e.g. /bin/echo -> busybox (Buildroot installs applets as
	 * symlinks). The argv (argv[0] = "echo") is kept, so busybox runs that applet. */
	for (int hop = 0; hop < 8; hop++) {
		const ove_lnx_file_t *lnk = &p->fs[idx];
		if ((file_mode(lnk) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFLNK)
			break;
		size_t tl = lnk->size;
		char abspath[128];
		if (!lnk->data || tl == 0 || tl >= sizeof(abspath))
			return -OVE_LNX_ENOENT;
		if (((const char *)lnk->data)[0] == '/') {
			memcpy(abspath, lnk->data, tl);
			abspath[tl] = 0;
		} else { /* relative to the symlink's own directory */
			const char *base = strrchr(lnk->path, '/');
			size_t dir = base ? (size_t)(base - lnk->path + 1) : 0;
			if (dir + tl >= sizeof(abspath))
				return -OVE_LNX_ENOENT;
			memcpy(abspath, lnk->path, dir);
			memcpy(abspath + dir, lnk->data, tl);
			abspath[dir + tl] = 0;
		}
		int ni = -1;
		for (int i = 0; i < p->fs_count; i++) {
			if (strcmp(p->fs[i].path, abspath) == 0) {
				ni = i;
				break;
			}
		}
		if (ni < 0)
			return -OVE_LNX_ENOENT;
		idx = ni;
	}
	if ((file_mode(&p->fs[idx]) & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
		return -OVE_LNX_EACCES;

	int argc = 0;
	size_t off = 0;
	while (argv && argv[argc] && argc < OVE_LNX_EXEC_MAXARGS) {
		size_t n = strlen(argv[argc]) + 1;
		if (off + n > sizeof(p->exec_argv_buf))
			break;
		memcpy(p->exec_argv_buf + off, argv[argc], n);
		p->exec_argv[argc] = p->exec_argv_buf + off;
		off += n;
		argc++;
	}
	p->exec_argc = argc;
	p->exec_file_idx = idx;
	p->exec_pending = 1;
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
	case OVE_LNX_NR_open: /* legacy open(path, flags, mode): dirfd = cwd */
		return sys_openat(proc, OVE_LNX_AT_FDCWD, (const char *)(uintptr_t)a0, (int)a1);
	case OVE_LNX_NR_execve: /* (path, argv, envp); envp ignored for now */
		return sys_execve(proc, (const char *)(uintptr_t)a0, (char *const *)(uintptr_t)a1);
	case OVE_LNX_NR_openat:
		return sys_openat(proc, (int)a0, (const char *)(uintptr_t)a1, (int)a2);
	case OVE_LNX_NR_close:
		return sys_close(proc, (int)a0);
	case OVE_LNX_NR_pipe:
		return sys_pipe(proc, (int *)(uintptr_t)a0);
	case OVE_LNX_NR_dup:
		return sys_dup(proc, (int)a0);
	case OVE_LNX_NR_dup2: /* dup3 ignores its flags arg here */
	case OVE_LNX_NR_dup3:
		return sys_dup2(proc, (int)a0, (int)a1);
	case OVE_LNX_NR_lseek:
		return sys_lseek(proc, (int)a0, a1, (int)a2);
	case OVE_LNX_NR_fstat64:
		return sys_fstat64(proc, (int)a0, (void *)(uintptr_t)a1);
	case OVE_LNX_NR_fcntl: /* old 32-bit fcntl: same dispatch as fcntl64 here */
	case OVE_LNX_NR_fcntl64: {
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s)
			return -OVE_LNX_EBADF;
		if ((int)a1 == OVE_LNX_F_DUPFD || (int)a1 == OVE_LNX_F_DUPFD_CLOEXEC) {
			/* Duplicate to the lowest free fd >= arg. The shell asks for a high
			 * fd (>=255) for its interactive fd; our table is small, so a too-high
			 * arg falls back to any free fd (the shell tolerates a low one and
			 * relocates it if needed). */
			int from = (int)a2;
			if (from < 0 || from >= OVE_LNX_MAX_FDS)
				from = 0;
			for (int nfd = from; nfd < OVE_LNX_MAX_FDS; nfd++) {
				if (proc->fds[nfd].kind == OVE_LNX_FD_FREE) {
					proc->fds[nfd] = *s;
					return nfd;
				}
			}
			return -OVE_LNX_EMFILE;
		}
		/* F_GETFD/SETFD/GETFL/SETFL: benign for stdio/dup probing. */
		return 0;
	}
	case OVE_LNX_NR_getdents64:
		return sys_getdents64(proc, (int)a0, (void *)(uintptr_t)a1, (size_t)a2);
	case OVE_LNX_NR_statx: /* (dirfd, path, flags, mask, buf); mask ignored */
		return sys_statx(proc, (int)a0, (const char *)(uintptr_t)a1, (int)a2,
				 (void *)(uintptr_t)a4);
	case OVE_LNX_NR_exit:
	case OVE_LNX_NR_exit_group:
		return sys_exit(proc, (int)a0);
	/* libc-init / identity stubs: enough for a static uClibc program to start. */
	case OVE_LNX_NR_getpid:
		return proc->pid;
	case OVE_LNX_NR_getppid:
		return proc->ppid;
	case OVE_LNX_NR_getcwd: {
		/* getcwd(buf, size): the rootfs has a single "/" working directory.
		 * The raw syscall writes the path and returns its length incl. NUL. */
		char *cwd = (char *)(uintptr_t)a0;
		if (!cwd)
			return -OVE_LNX_EFAULT;
		if ((size_t)a1 < 2)
			return -OVE_LNX_ERANGE;
		cwd[0] = '/';
		cwd[1] = '\0';
		return 2;
	}
	case OVE_LNX_NR_prctl:
	case OVE_LNX_NR_setpgid:
		return 0; /* process-control setup accepted (inert) */
	case OVE_LNX_NR_gettid:
		return proc->pid; /* single-threaded: tid == pid */
	case OVE_LNX_NR_uname: {
		/* struct utsname: 6 fixed 65-byte fields (sysname, nodename, release,
		 * version, machine, domainname). The shell reads these at startup. */
		char *u = (char *)(uintptr_t)a0;
		if (!u)
			return -OVE_LNX_EFAULT;
		static const char *const f[6] = {"Linux",   "overtos", "6.1.0",
						 "oveRTOS", "armv7l",  "(none)"};
		memset(u, 0, 6 * 65);
		for (int i = 0; i < 6; i++) {
			size_t l = strlen(f[i]);
			memcpy(u + i * 65, f[i], l + 1);
		}
		return 0;
	}
	case OVE_LNX_NR_rt_sigaction: {
		/* Record the per-signal disposition; the engine seam delivers it.
		 * struct sigaction: sa_handler@0, sa_flags@4, sa_restorer@8. */
		int sig = (int)a0;
		if (sig < 1 || sig >= OVE_LNX_NSIG)
			return -OVE_LNX_EINVAL;
		const uint32_t *act = (const uint32_t *)(uintptr_t)a1;
		uint32_t *oact = (uint32_t *)(uintptr_t)a2;
		if (oact) {
			oact[0] = (uint32_t)proc->sig_handler[sig];
			oact[2] = (uint32_t)proc->sig_restorer[sig];
		}
		if (act) {
			proc->sig_handler[sig] = act[0];
			proc->sig_restorer[sig] = act[2];
		}
		return 0;
	}
	case OVE_LNX_NR_poll:
	case OVE_LNX_NR_ppoll_time64: {
		/* The console is always ready and rootfs files never block, so report
		 * the requested readiness immediately (the shell polls stdin). ppoll's
		 * timeout/sigmask args are ignored; only fds (a0) + nfds (a1) matter. */
		ove_lnx_pollfd *pfds = (ove_lnx_pollfd *)(uintptr_t)a0;
		unsigned nfds = (unsigned)a1;
		int ready = 0;
		for (unsigned i = 0; i < nfds; i++) {
			pfds[i].revents = 0;
			if (!fd_slot(proc, pfds[i].fd))
				continue;
			pfds[i].revents = pfds[i].events & (OVE_LNX_POLLIN | OVE_LNX_POLLOUT);
			if (pfds[i].revents)
				ready++;
		}
		return ready;
	}
	case OVE_LNX_NR_wait4: {
		/* Reap the oldest queued child (FIFO); the wait-status word encodes a
		 * normal exit as exit_code << 8. a1 is the int* status, else NULL. */
		if (proc->child_count == 0)
			return -OVE_LNX_ECHILD;
		int pid = proc->child_pid[0];
		int code = proc->child_status[0];
		for (int i = 1; i < proc->child_count; i++) {
			proc->child_pid[i - 1] = proc->child_pid[i];
			proc->child_status[i - 1] = proc->child_status[i];
		}
		proc->child_count--;
		int *status = (int *)(uintptr_t)a1;
		if (status)
			*status = (code & 0xff) << 8;
		return pid;
	}
	case OVE_LNX_NR_getuid32:
	case OVE_LNX_NR_geteuid32:
	case OVE_LNX_NR_getgid32:
	case OVE_LNX_NR_getegid32:
		return 0; /* run as root */
	case OVE_LNX_NR_ioctl: {
		/* Make the console fds look like a tty so the shell goes interactive
		 * (isatty → prompt + line editing). Non-console fds are not ttys. */
		ove_lnx_fd_t *tty = fd_slot(proc, (int)a0);
		if (!tty || tty->kind != OVE_LNX_FD_CONSOLE)
			return -OVE_LNX_ENOTTY;
		switch ((unsigned long)a1) {
		case OVE_LNX_TCGETS: {
			ove_lnx_termios *t = (ove_lnx_termios *)(uintptr_t)a2;
			if (!t)
				return -OVE_LNX_EFAULT;
			memset(t, 0, sizeof(*t));
			t->c_iflag = OVE_LNX_ICRNL;
			t->c_oflag = OVE_LNX_OPOST | OVE_LNX_ONLCR;
			t->c_cflag = OVE_LNX_CS8 | OVE_LNX_CREAD;
			t->c_lflag = OVE_LNX_ICANON | OVE_LNX_ECHO | OVE_LNX_ISIG;
			t->c_cc[OVE_LNX_VINTR] = 3;	/* ^C */
			t->c_cc[OVE_LNX_VERASE] = 0x7f; /* DEL */
			t->c_cc[OVE_LNX_VEOF] = 4;	/* ^D */
			t->c_cc[OVE_LNX_VMIN] = 1;
			return 0;
		}
		case OVE_LNX_TCSETS:
		case OVE_LNX_TCSETSW:
		case OVE_LNX_TCSETSF:
			return 0; /* accept mode changes; the console echo is the engine's job */
		case OVE_LNX_TIOCGWINSZ: {
			ove_lnx_winsize *w = (ove_lnx_winsize *)(uintptr_t)a2;
			if (!w)
				return -OVE_LNX_EFAULT;
			w->ws_row = 24;
			w->ws_col = 80;
			w->ws_xpixel = 0;
			w->ws_ypixel = 0;
			return 0;
		}
		default:
			return -OVE_LNX_ENOTTY;
		}
	}
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
