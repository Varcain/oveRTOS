/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_LINUX)

#include "ove/linux/stats.h"
#include "ove/linux/syscall.h"
#include "ove/time.h"
#if defined(CONFIG_OVE_LINUX_DEV)
#include "ove/linux/dev.h" /* /dev character-device routing (FD_DEV) */
#endif
#if defined(CONFIG_OVE_LINUX_NET)
#include "ove/linux/net.h" /* socket routing (FD_SOCKET) */
#endif

#include <string.h>

/* Set by reboot(2)/poweroff to stop the run loop; the common run loop observes
 * it (declared extern there). Defined here so the host syscall tests, which do
 * not link the run loop, still resolve the symbol. */
volatile int g_ove_lnx_halt;

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
 * a bounded ring buffer with concurrent producer/consumer (Phase D2). A read on an
 * empty pipe blocks while any write end is open (EOF only once all writers close); a
 * write on a full pipe blocks while a reader is open (-EPIPE once all readers close).
 * The run-loop coordinator parks/wakes the blocked proc — see ove_lnx_pipe_retry.
 */
#define OVE_LNX_NPIPE 4
/* Ring size: a bigger ring means a typical write/splice fits in fewer shots (no partial-write
 * park/resume round trip per chunk), so streaming throughput is copy-bound not coordinator-bound.
 * 4 KiB on FreeRTOS/NuttX (4 pipes × 4 KiB = 16 KiB .bss). Zephyr runs programs in per-program
 * K_USER MPU domains + privilege stacks that eat the STM32F746's internal SRAM, so 4 KiB there
 * overflows RAM by ~5 KiB — cap it at 2 KiB (still 2x the old 1 KiB). */
#if defined(CONFIG_OVE_RTOS_ZEPHYR)
#define OVE_LNX_PIPE_BUF 2048
#else
#define OVE_LNX_PIPE_BUF 4096
#endif
typedef struct {
	uint8_t buf[OVE_LNX_PIPE_BUF];
	size_t rpos;  /* ring read index [0, BUF) */
	size_t wpos;  /* ring write index [0, BUF) */
	size_t count; /* bytes currently buffered */
	int used;
} ove_lnx_pipe_t;
static ove_lnx_pipe_t g_pipes[OVE_LNX_NPIPE];

/* Count a pipe's open read/write ends across ALL live procs' fd tables (a pipe end
 * is open in every proc that holds an fd onto it — inherited across fork, dropped on
 * close/exit). Recomputed on demand → no per-fd refcount bookkeeping to keep in sync. */
/* Weak fallbacks so the host syscall test (which links this layer but not the run
 * loop) resolves these; the run loop supplies the strong on-target versions. The
 * host test never exercises pipes, so pipe_ends is never actually called there. */
__attribute__((weak)) ove_lnx_proc_t *ove_lnx_proc_table(void)
{
	return NULL;
}
__attribute__((weak)) int ove_lnx_proc_nslot(void)
{
	return 0;
}

/* The shared read-only rootfs span [lo,hi): a program's .rodata (its FDPIC text is shared in-place
 * from the embedded cpio) lives here, so a READ-source user pointer may legitimately point into it.
 * Weak fallback (empty range) so the host test links; the run loop supplies the strong version. */
__attribute__((weak)) void ove_lnx_rootfs_bounds(uintptr_t *lo, uintptr_t *hi)
{
	*lo = 0;
	*hi = 0;
}

/* ---- user-pointer validation (access_ok) ----------------------------------
 * The syscall handlers run PRIVILEGED, so an unchecked user pointer would let a program make the
 * KERNEL read/write kernel, device, or unmapped memory on its behalf — a confused deputy the
 * per-program MPU cannot stop. Every syscall that dereferences a user pointer must first prove the
 * whole [ptr, ptr+len) lies inside the calling program's own writable memory (its image region or
 * dynamic arena), or — for a read SOURCE only — the shared read-only rootfs. */

/* Upper bound of the valid range that CONTAINS `a`, or 0 if `a` is in none. */
static uintptr_t user_range_hi(const ove_lnx_proc_t *p, uintptr_t a, int write)
{
	if (a >= p->region_lo && a < p->region_hi)
		return p->region_hi;
	if (p->pool_hi > p->pool_lo && a >= p->pool_lo && a < p->pool_hi)
		return p->pool_hi;
#if defined(CONFIG_OVE_LINUX_DEV)
	/* A mapped device buffer (framebuffer, P3) is RW-valid for the program. */
	for (int i = 0; i < 2; i++)
		if (p->dev_map_hi[i] > p->dev_map_lo[i] && a >= p->dev_map_lo[i] &&
		    a < p->dev_map_hi[i])
			return p->dev_map_hi[i];
#endif
	if (!write) {
		uintptr_t rlo, rhi;
		ove_lnx_rootfs_bounds(&rlo, &rhi);
		if (rhi > rlo && a >= rlo && a < rhi)
			return rhi;
	}
	return 0;
}

/* True iff [ptr, ptr+len) is wholly readable (write==0) or writable (write==1) by program `p`.
 * Non-static so the host unit tests exercise the boundary/overflow logic directly. */
int user_ok(const ove_lnx_proc_t *p, const void *ptr, size_t len, int write)
{
	uintptr_t a = (uintptr_t)ptr, end = a + len;
	if (len == 0)
		return 1; /* zero length: nothing is dereferenced */
	if (end < a)
		return 0; /* ptr+len wrapped the address space */
	uintptr_t hi = user_range_hi(p, a, write);
	return hi != 0 && end <= hi;
}

/* strlen of a user string, or -EFAULT if it is not NUL-terminated wholly within a valid readable
 * range (so a later strlen/copy can't walk off the region into kernel memory). Bounded by `max`.
 * Non-static so the host unit tests exercise the terminated/unterminated/at-edge cases directly. */
long user_strnlen(const ove_lnx_proc_t *p, const char *s, size_t max)
{
	uintptr_t a = (uintptr_t)s;
	uintptr_t hi = user_range_hi(p, a, 0);
	if (!hi)
		return -OVE_LNX_EFAULT;
	size_t avail = (size_t)(hi - a);
	size_t lim = avail < max ? avail : max;
	for (size_t i = 0; i < lim; i++)
		if (s[i] == '\0')
			return (long)i;
	return -OVE_LNX_EFAULT; /* no NUL within the range / max */
}

static void pipe_ends(int pi, int *readers, int *writers)
{
	*readers = 0;
	*writers = 0;
	ove_lnx_proc_t *tab = ove_lnx_proc_table();
	int n = ove_lnx_proc_nslot();
	if (!tab)
		return;
	for (int s = 0; s < n; s++) {
		if (!tab[s].alive)
			continue;
		for (int fd = 0; fd < OVE_LNX_MAX_FDS; fd++)
			if (tab[s].fds[fd].kind == OVE_LNX_FD_PIPE && tab[s].fds[fd].file_idx == pi)
				(tab[s].fds[fd].rw ? (*writers)++ : (*readers)++);
	}
}

/* Drain up to len bytes from pipe pi. >0 = bytes read; 0 = EOF (empty, no writers);
 * -EAGAIN = empty but a writer is open (caller should block). */
static long pipe_try_read(int pi, void *buf, size_t len)
{
	ove_lnx_pipe_t *pp = &g_pipes[pi];
	if (pp->count == 0) {
		int rd, wr;
		pipe_ends(pi, &rd, &wr);
		return wr > 0 ? -OVE_LNX_EAGAIN : 0;
	}
	if (len > pp->count)
		len = pp->count;
	uint8_t *out = (uint8_t *)buf;
	size_t n1 = OVE_LNX_PIPE_BUF - pp->rpos; /* contiguous bytes to the ring end */
	if (n1 > len)
		n1 = len;
	memcpy(out, &pp->buf[pp->rpos], n1);
	memcpy(out + n1, &pp->buf[0], len - n1); /* wrapped tail (len-n1 may be 0 = no-op) */
	pp->rpos = (pp->rpos + len) % OVE_LNX_PIPE_BUF;
	pp->count -= len;
	return (long)len;
}

/* Append up to len bytes to pipe pi. >0 = bytes written; -EPIPE = no readers open
 * (broken pipe); -EAGAIN = full but a reader is open (caller should block). */
static long pipe_try_write(int pi, const void *buf, size_t len)
{
	ove_lnx_pipe_t *pp = &g_pipes[pi];
	int rd, wr;
	pipe_ends(pi, &rd, &wr);
	if (rd == 0)
		return -OVE_LNX_EPIPE;
	size_t space = OVE_LNX_PIPE_BUF - pp->count;
	if (space == 0)
		return -OVE_LNX_EAGAIN;
	if (len > space)
		len = space;
	const uint8_t *in = (const uint8_t *)buf;
	size_t n1 = OVE_LNX_PIPE_BUF - pp->wpos; /* contiguous space to the ring end */
	if (n1 > len)
		n1 = len;
	memcpy(&pp->buf[pp->wpos], in, n1);
	memcpy(&pp->buf[0], in + n1, len - n1); /* wrapped tail (len-n1 may be 0 = no-op) */
	pp->wpos = (pp->wpos + len) % OVE_LNX_PIPE_BUF;
	pp->count += len;
	return (long)len;
}

/* Retry a parked pipe read/write for the run-loop coordinator (declared in syscall.h). */
long ove_lnx_pipe_retry(ove_lnx_proc_t *p)
{
	if (p->pipe_wait == 1)
		return pipe_try_read(p->pipe_idx, (void *)p->pipe_buf, p->pipe_len);
	if (p->pipe_wait == 2)
		return pipe_try_write(p->pipe_idx, (const void *)p->pipe_buf, p->pipe_len);
	return 0;
}

/*
 * Writable VFS overlaid on the read-only CPIO rootfs: regular files, directories
 * (mkdir), and symlinks (ln -s) created at runtime live here (e.g. `mkdir /tmp/d`,
 * `echo x > /tmp/f`). Nodes are global kernel state (shared across processes, like
 * pipes), keyed by absolute path. File bytes come from a bump-allocated pool;
 * growth re-allocates (the old block leaks — bounded by the pool, ENOSPC when
 * exhausted), and unlink/rmdir frees the node (not its bytes). Not a tree: a node
 * is "in" a directory iff its path is one component below the dir's path.
 */
#define OVE_LNX_FD_TMPFS 4
#define OVE_LNX_NWNODE 32
#define OVE_LNX_WFS_POOL (64u * 1024u)
typedef struct {
	char path[OVE_LNX_PATH_MAX]; /* absolute, normalized */
	uint32_t mode;		     /* S_IFREG|perms, S_IFDIR|perms, or S_IFLNK */
	uint8_t *data;		     /* file/symlink bytes (pool); NULL when empty */
	size_t size;
	size_t cap;
	int used;
} ove_lnx_wnode_t;
static ove_lnx_wnode_t g_wnodes[OVE_LNX_NWNODE];
static uint8_t g_wfs_pool[OVE_LNX_WFS_POOL];
static size_t g_wfs_off;

static uint8_t *wfs_alloc(size_t n)
{
	n = (n + 7u) & ~(size_t)7u;
	if (g_wfs_off + n > sizeof(g_wfs_pool))
		return NULL;
	uint8_t *p = g_wfs_pool + g_wfs_off;
	g_wfs_off += n;
	return p;
}

/* Find a writable node by absolute path (any type), or -1. */
static int wfs_find(const char *abspath)
{
	for (int i = 0; i < OVE_LNX_NWNODE; i++)
		if (g_wnodes[i].used && strcmp(g_wnodes[i].path, abspath) == 0)
			return i;
	return -1;
}

/* Allocate a node for abspath with mode; -1 if the table is full / path too long. */
static int wfs_create(const char *abspath, uint32_t mode)
{
	if (strlen(abspath) >= OVE_LNX_PATH_MAX)
		return -1;
	for (int i = 0; i < OVE_LNX_NWNODE; i++)
		if (!g_wnodes[i].used) {
			strcpy(g_wnodes[i].path, abspath);
			g_wnodes[i].mode = mode;
			g_wnodes[i].data = NULL;
			g_wnodes[i].size = 0;
			g_wnodes[i].cap = 0;
			g_wnodes[i].used = 1;
			return i;
		}
	return -1;
}

/* Ensure node i can hold `need` bytes (grows from the pool; old block leaks). */
static int wfs_reserve(int i, size_t need)
{
	ove_lnx_wnode_t *w = &g_wnodes[i];
	if (need <= w->cap)
		return 0;
	size_t ncap = (need + 255u) & ~(size_t)255u;
	uint8_t *nd = wfs_alloc(ncap);
	if (!nd)
		return -1;
	if (w->data && w->size)
		memcpy(nd, w->data, w->size);
	w->data = nd;
	w->cap = ncap;
	return 0;
}

/* Synthetic /proc fd backing (content generated on open; see proc_* below). */
#define OVE_LNX_FD_PROC 5
#define OVE_LNX_NPROCF 12
#define OVE_LNX_PROCBUF 1024
static struct {
	char path[OVE_LNX_PATH_MAX];
	char buf[OVE_LNX_PROCBUF];
	size_t len;
	int is_dir;
	int used;
} g_procf[OVE_LNX_NPROCF];

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
	proc->pid = 1;	    /* the initial program is pid 1 (ppid 0); fork assigns the rest */
	proc->cwd[0] = '/'; /* start at the root directory */
	proc->cwd[1] = '\0';
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
			  const char *const envp[], int fdpic, uintptr_t phdr, int phnum,
			  uintptr_t entry, uintptr_t at_base)
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

	/* FDPIC programs use the STANDARD ELF inline stack (the crt reads argc at sp,
	 * argv[] inline at sp+4, then computes envp = &argv[argc+1]): argc, argv[0..],
	 * NULL, envp[0..], NULL, auxv. (bFLT instead wants a 3-word
	 * argc/argv-ptr/envp-ptr header — see below.) */
	if (fdpic) {
		size_t nwords = 1 + (size_t)argc + 1 + (size_t)envc + 1 + 16;
		uintptr_t *hdr =
			(uintptr_t *)((uintptr_t)(sp - nwords * sizeof(uintptr_t)) & ~(uintptr_t)7);
		if ((uint8_t *)hdr < floor)
			return NULL;
		size_t k = 0;
		hdr[k++] = (uintptr_t)argc;
		for (int i = 0; i < argc; i++)
			hdr[k++] = argp[i];
		hdr[k++] = 0; /* argv[] terminator */
		for (int i = 0; i < envc; i++)
			hdr[k++] = envpp[i];
		hdr[k++] = 0; /* envp[] terminator */
		/* auxv — the FDPIC crt locates PT_TLS / the segments via AT_PHDR/AT_PHNUM. */
		hdr[k++] = OVE_LNX_AT_PHDR;
		hdr[k++] = phdr;
		hdr[k++] = OVE_LNX_AT_PHENT;
		hdr[k++] = 32; /* sizeof(Elf32_Phdr) */
		hdr[k++] = OVE_LNX_AT_PHNUM;
		hdr[k++] = (uintptr_t)phnum;
		hdr[k++] = OVE_LNX_AT_BASE;
		hdr[k++] = at_base; /* ld.so's load base for a dynamic exec; 0 when static */
		hdr[k++] = OVE_LNX_AT_ENTRY;
		hdr[k++] = entry; /* the program's own entry (AT_ENTRY), even when ld.so runs first */
		hdr[k++] = OVE_LNX_AT_PAGESZ;
		hdr[k++] = 4096;
		hdr[k++] = OVE_LNX_AT_RANDOM;
		hdr[k++] = (uintptr_t)rnd;
		hdr[k++] = OVE_LNX_AT_NULL;
		hdr[k++] = 0;
		return hdr; /* SP -> argc, argv[] inline */
	}

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
	if (!user_ok(p, buf, len, 0)) /* the kernel READS buf → reject a bad source pointer */
		return -OVE_LNX_EFAULT;
#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		return ove_lnx_dev_write(p, s->file_idx, buf, len);
#endif
#if defined(CONFIG_OVE_LINUX_NET)
	if (s->kind == OVE_LNX_FD_SOCKET)
		return ove_lnx_sock_send(p, s->file_idx, buf, len, 0, NULL, 0);
#endif
	/* A pipe write end appends to the shared ring; blocks when full (reader open). */
	if (s->kind == OVE_LNX_FD_PIPE) {
		if (s->rw != 1)
			return -OVE_LNX_EBADF;
		long r = pipe_try_write(s->file_idx, buf, len);
		if (r == -OVE_LNX_EAGAIN) { /* full but a reader is open → park + retry */
			p->pipe_wait = 2;
			p->pipe_idx = s->file_idx;
			p->pipe_buf = (uintptr_t)buf;
			p->pipe_len = len;
			return 0; /* dispatch parks; coordinator completes via ove_lnx_pipe_retry */
		}
		if (r == -OVE_LNX_EPIPE && /* no readers: SIGPIPE — default terminates the writer */
		    p->sig_handler[OVE_LNX_SIGPIPE] != OVE_LNX_SIG_IGN) {
			p->exited = 1;
			p->exit_status = 128 + OVE_LNX_SIGPIPE;
		}
		return r; /* bytes written, or -EPIPE (no readers; writer exits unless it ignores it) */
	}
	/* A writable-node file write copies into its (growable) buffer at the offset. */
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_wnode_t *t = &g_wnodes[s->file_idx];
		if ((t->mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
			return -OVE_LNX_EBADF;
		if (wfs_reserve(s->file_idx, s->offset + len) != 0)
			return -OVE_LNX_EFBIG; /* writable-fs pool exhausted */
		memcpy(t->data + s->offset, buf, len);
		s->offset += len;
		if (s->offset > t->size)
			t->size = s->offset;
		return (long)len;
	}
	if (s->kind == OVE_LNX_FD_CONSOLE && s->file_idx == 3)
		return (long)len; /* /dev/null: discard */
	/* Only output consoles are writable (file_idx != 0); the rootfs is read-only. */
	if (s->kind != OVE_LNX_FD_CONSOLE || s->file_idx == 0 || !p->write_fn)
		return -OVE_LNX_EBADF;
	return p->write_fn(p->io_ctx, fd, buf, len);
}

static long sys_writev(ove_lnx_proc_t *p, int fd, const ove_lnx_iovec *iov, int iovcnt)
{
	/* Any fd sys_write accepts: console, socket (uClibc stdio flushes a socket via
	 * writev — this is how wget sends its HTTP request), device, file. sys_write
	 * validates the fd (EBADF) and routes by kind. */
	if (iovcnt < 0)
		return -OVE_LNX_EINVAL;
	if (iovcnt && !user_ok(p, iov, (size_t)iovcnt * sizeof(*iov), 0))
		return -OVE_LNX_EFAULT; /* the iov array itself; each iov_base is checked in sys_write */

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
	if (!user_ok(p, buf, len, 1)) /* the kernel WRITES buf → reject a bad destination pointer */
		return -OVE_LNX_EFAULT;

#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		return ove_lnx_dev_read(p, s->file_idx, buf, len);
#endif
#if defined(CONFIG_OVE_LINUX_NET)
	if (s->kind == OVE_LNX_FD_SOCKET)
		return ove_lnx_sock_recv(p, s->file_idx, buf, len, 0, NULL, NULL);
#endif

	if (s->kind == OVE_LNX_FD_CONSOLE) {
		if (s->file_idx == 1) /* output consoles (stdout/stderr) are not readable */
			return -OVE_LNX_EBADF;
		if (s->file_idx == 3) /* /dev/null */
			return 0;     /* EOF */
		if (!p->read_fn)
			return 0; /* EOF */
		return p->read_fn(p->io_ctx, fd, buf, len);
	}

	/* A pipe read end drains the shared ring; blocks while empty + a writer is open,
	 * EOF (0) once all writers have closed. */
	if (s->kind == OVE_LNX_FD_PIPE) {
		if (s->rw != 0)
			return -OVE_LNX_EBADF;
		long r = pipe_try_read(s->file_idx, buf, len);
		if (r == -OVE_LNX_EAGAIN) { /* empty but a writer is open → park + retry */
			p->pipe_wait = 1;
			p->pipe_idx = s->file_idx;
			p->pipe_buf = (uintptr_t)buf;
			p->pipe_len = len;
			return 0;
		}
		return r; /* bytes read, or 0 (EOF) */
	}

	/* A writable-node file read returns bytes from its buffer at the fd offset. */
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_wnode_t *t = &g_wnodes[s->file_idx];
		if ((t->mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
			return -OVE_LNX_EISDIR;
		if (s->offset >= t->size)
			return 0; /* EOF */
		size_t n = t->size - s->offset;
		if (n > len)
			n = len;
		memcpy(buf, t->data + s->offset, n);
		s->offset += n;
		return (long)n;
	}

	/* A /proc file read returns bytes from the content generated at open. */
	if (s->kind == OVE_LNX_FD_PROC) {
		if (g_procf[s->file_idx].is_dir)
			return -OVE_LNX_EISDIR;
		size_t plen = g_procf[s->file_idx].len;
		if ((size_t)s->offset >= plen)
			return 0; /* EOF */
		size_t n = plen - s->offset;
		if (n > len)
			n = len;
		memcpy(buf, g_procf[s->file_idx].buf + s->offset, n);
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

/*
 * pread64(fd, buf, count, offset): a positioned read that does NOT move the fd offset.
 * ld.so uses it to pull each PT_LOAD of a .so out of the rootfs into the anonymous memory
 * it mapped (the NOMMU path: MAP_FIXED-file mmap fails, so it mmaps anon + preads). Only
 * regular (seekable) files are supported — console/pipe return ESPIPE.
 */
static long sys_pread(ove_lnx_proc_t *p, int fd, void *buf, size_t len, uint32_t off)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (!user_ok(p, buf, len, 1))
		return -OVE_LNX_EFAULT;

#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		return ove_lnx_dev_pread(p, s->file_idx, buf, len, off);
#endif
	const uint8_t *data;
	size_t size;
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_wnode_t *t = &g_wnodes[s->file_idx];
		if ((t->mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
			return -OVE_LNX_EISDIR;
		data = (const uint8_t *)t->data;
		size = t->size;
	} else if (s->kind == OVE_LNX_FD_PROC) {
		if (g_procf[s->file_idx].is_dir)
			return -OVE_LNX_EISDIR;
		data = (const uint8_t *)g_procf[s->file_idx].buf;
		size = g_procf[s->file_idx].len;
	} else if (s->kind == OVE_LNX_FD_CONSOLE || s->kind == OVE_LNX_FD_PIPE) {
		return -OVE_LNX_ESPIPE; /* not seekable */
	} else {
		const ove_lnx_file_t *f = &p->fs[s->file_idx];
		if ((file_mode(f) & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
			return -OVE_LNX_EISDIR;
		data = (const uint8_t *)f->data;
		size = f->size;
	}
	if ((size_t)off >= size)
		return 0; /* EOF */
	size_t n = size - off;
	if (n > len)
		n = len;
	memcpy(buf, data + off, n);
	return (long)n;
}

/*
 * pwrite64(fd, buf, count, offset): a positioned write that does NOT move the fd offset.
 * LVGL's fbdev driver (LV_LINUX_FBDEV_MMAP=0) writes each framebuffer scanline this way.
 * Device fds route to the driver; the writable overlay writes at the offset; the read-only
 * rootfs and console/pipe are not positioned-writable (ESPIPE).
 */
static long sys_pwrite(ove_lnx_proc_t *p, int fd, const void *buf, size_t len, uint32_t off)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (!user_ok(p, buf, len, 0))
		return -OVE_LNX_EFAULT;
#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		return ove_lnx_dev_pwrite(p, s->file_idx, buf, len, off);
#endif
	if (s->kind == OVE_LNX_FD_TMPFS) {
		ove_lnx_wnode_t *t = &g_wnodes[s->file_idx];
		if ((t->mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
			return -OVE_LNX_EBADF;
		if (wfs_reserve(s->file_idx, (size_t)off + len) != 0)
			return -OVE_LNX_EFBIG;
		memcpy(t->data + off, buf, len);
		if ((size_t)off + len > t->size)
			t->size = (size_t)off + len;
		return (long)len;
	}
	return -OVE_LNX_ESPIPE; /* console / pipe / read-only rootfs */
}

/*
 * mprotect: a no-op on NOMMU (there is no per-page protection). ld.so calls it to apply
 * PT_GNU_RELRO hardening; it must succeed rather than fault the loader.
 */
static long sys_mprotect(uintptr_t addr, size_t len, int prot)
{
	(void)addr;
	(void)len;
	(void)prot;
	return 0;
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
static long sys_mmap2(ove_lnx_proc_t *p, uintptr_t addr, size_t len, int prot, int flags, int fd,
		      uint32_t pgoff)
{
	(void)addr;
	if (len == 0)
		return -OVE_LNX_EINVAL;

	/* Text-sharing: a read-only file map of a rootfs file whose whole extent lies within the file
	 * is returned IN-PLACE (zero-copy). FDPIC text is pure PIC — its relocations land in the
	 * per-process GOT/data, never the shared text — so every dynamic process shares ONE libc.so
	 * text copy (the embedded cpio bytes) instead of its own ~358K arena copy. Privileged engines
	 * (FreeRTOS/NuttX) reach the cpio directly; Zephyr embeds the cpio in an executable .text
	 * subsection (.text.ove_rootfs), covered by the kernel's user-RX .text MPU region, so the
	 * unprivileged program reads + executes the in-place text there too — no separate partition. */
	if (!(flags & OVE_LNX_MAP_ANONYMOUS) && fd >= 0 && !(prot & 0x2 /* PROT_WRITE */)) {
		ove_lnx_fd_t *s = fd_slot(p, fd);
		if (s && s->kind == OVE_LNX_FD_FILE) {
			const ove_lnx_file_t *f = &p->fs[s->file_idx];
			if ((size_t)pgoff * 4096u + len <= f->size)
				return (long)(uintptr_t)(f->data + (size_t)pgoff * 4096u);
		}
	}

#if defined(CONFIG_OVE_LINUX_DEV)
	/* Device mmap (P3): a real /dev fd with a driver .mmap op (e.g. /dev/fb0) is mapped to
	 * the device's own buffer — ove_lnx_dev_mmap parks on DEVW_MMAP and the coordinator
	 * installs the unprivileged MPU region + resumes with the mapped address. Devices
	 * without an .mmap op return -ENODEV and fall through to the anonymous-arena copy. */
	if (fd >= 0 && !(flags & OVE_LNX_MAP_ANONYMOUS)) {
		ove_lnx_fd_t *s = fd_slot(p, fd);
		if (s && s->kind == OVE_LNX_FD_DEV) {
			long r = ove_lnx_dev_mmap(p, s->file_idx, len, pgoff);
			if (r != -OVE_LNX_ENODEV)
				return r;
		}
	}
#endif

	void *m = ove_arena_alloc(p->arena, len);
	if (!m)
		return -OVE_LNX_ENOMEM;
	memset(m, 0, len); /* anon reads as zero; also zero-fills a file map's bss tail */
	if (!(flags & OVE_LNX_MAP_ANONYMOUS) && fd >= 0) {
		/* File-backed mapping: ld.so loads a .so's read-only segment (the symtab/hash/
		 * text) this way on NOMMU — read the file's bytes at the page offset into the
		 * freshly-allocated block. (Anonymous maps ignore the fd.) */
		long r = sys_pread(p, fd, m, len, pgoff * 4096u);
		if (r < 0)
			return r;
	}
	return (long)(uintptr_t)m;
}

/*
 * munmap is a no-op for now: the bump/free-list arena is reclaimed wholesale at
 * process teardown, and a partial unmap of an arena chunk could corrupt the
 * free list. Tracking mmap extents for precise release is a later step.
 */
static long sys_munmap(ove_lnx_proc_t *p, uintptr_t addr, size_t len)
{
	(void)len;
	/* Reclaim the mapping. uClibc's malloc (MALLOC=y) grows its heap with anonymous
	 * mmap and munmaps freed blocks; without this the process arena grows monotonically
	 * across malloc/free churn (getaddrinfo + stdio + wget headers) and exhausts —
	 * "wget: out of memory". In-place file/device maps are not arena-owned, so
	 * ove_arena_free ignores them (its ove_arena_owns bounds-check). */
	if (p && p->arena)
		ove_arena_free(p->arena, (void *)addr);
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

/*
 * Collapse ".", "..", and duplicate/trailing slashes in absolute path `in` into
 * out[outlen]. Returns 0, or -ENAMETOOLONG on overflow.
 */
static long normalize_abs(const char *in, char *out, size_t outlen)
{
	size_t ol = 0;
	out[0] = '\0';
	const char *s = in;
	while (*s) {
		while (*s == '/')
			s++;
		if (!*s)
			break;
		const char *seg = s;
		while (*s && *s != '/')
			s++;
		size_t seglen = (size_t)(s - seg);
		if (seglen == 1 && seg[0] == '.') {
			continue; /* "." → current dir */
		}
		if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
			while (ol > 0 && out[ol - 1] != '/') /* drop last component */
				ol--;
			if (ol > 0)
				ol--; /* drop the separating '/' */
			out[ol] = '\0';
			continue;
		}
		if (ol + 1 + seglen >= outlen)
			return -OVE_LNX_ENAMETOOLONG;
		out[ol++] = '/';
		memcpy(out + ol, seg, seglen);
		ol += seglen;
		out[ol] = '\0';
	}
	if (ol == 0) { /* everything collapsed away → root */
		out[0] = '/';
		out[1] = '\0';
	}
	return 0;
}

/*
 * Resolve `in` (absolute, or relative to the process cwd) into a normalized
 * absolute path in out[outlen]. Returns 0, or -ENAMETOOLONG on overflow.
 */
static long resolve_path(const ove_lnx_proc_t *p, const char *in, char *out, size_t outlen)
{
	/* Every path syscall funnels through here, so one check guards them all: reject a path pointer
	 * that isn't a NUL-terminated string wholly inside the program's memory (-EFAULT) before any
	 * deref — else a bad `in` faults the kernel or walks a strlen off the region. */
	if (user_strnlen(p, in, OVE_LNX_PATH_MAX) < 0)
		return -OVE_LNX_EFAULT;
	char joined[OVE_LNX_PATH_MAX];
	size_t jl = 0;
	if (in[0] != '/') { /* prefix the cwd (which is absolute + normalized) */
		for (const char *c = p->cwd; *c; c++) {
			if (jl + 2 >= sizeof(joined))
				return -OVE_LNX_ENAMETOOLONG;
			joined[jl++] = *c;
		}
		joined[jl++] = '/';
	}
	for (const char *c = in; *c; c++) {
		if (jl + 1 >= sizeof(joined))
			return -OVE_LNX_ENAMETOOLONG;
		joined[jl++] = *c;
	}
	joined[jl] = '\0';
	return normalize_abs(joined, out, outlen);
}

/* Find the rootfs index for an absolute path in (fs,count), or -1. */
static int fsx_lookup(const ove_lnx_file_t *fs, int count, const char *abspath)
{
	for (int i = 0; i < count; i++)
		if (strcmp(fs[i].path, abspath) == 0)
			return i;
	return -1;
}

/*
 * Follow symlinks from rootfs index `idx` (up to 8 hops), normalizing each
 * target against the link's own directory (so e.g. /sbin/init -> ../bin/busybox
 * resolves to /bin/busybox). Returns the final non-symlink index, or -1.
 */
static int fsx_follow(const ove_lnx_file_t *fs, int count, int idx)
{
	for (int hop = 0; hop < 8 && idx >= 0; hop++) {
		const ove_lnx_file_t *lnk = &fs[idx];
		if ((file_mode(lnk) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFLNK)
			return idx;
		const char *tgt = (const char *)lnk->data;
		size_t tl = lnk->size;
		if (!tgt || tl == 0)
			return -1;
		char raw[OVE_LNX_PATH_MAX], abs[OVE_LNX_PATH_MAX];
		size_t rl = 0;
		if (tgt[0] != '/') { /* relative to the link's own directory */
			const char *base = strrchr(lnk->path, '/');
			rl = base ? (size_t)(base - lnk->path + 1) : 0;
			if (rl >= sizeof(raw))
				return -1;
			memcpy(raw, lnk->path, rl);
		}
		if (rl + tl >= sizeof(raw))
			return -1;
		memcpy(raw + rl, tgt, tl);
		raw[rl + tl] = '\0';
		if (normalize_abs(raw, abs, sizeof(abs)) < 0)
			return -1;
		idx = fsx_lookup(fs, count, abs);
	}
	return idx;
}

/* Find the rootfs index for an absolute path, or -1. */
static int fs_lookup(const ove_lnx_proc_t *p, const char *abspath)
{
	return fsx_lookup(p->fs, p->fs_count, abspath);
}

static int fs_follow(const ove_lnx_proc_t *p, int idx)
{
	return fsx_follow(p->fs, p->fs_count, idx);
}

/*
 * Resolve an absolute path through (fs,count), following symlinks, to its target file's
 * bytes. Public so the run loop can locate the FDPIC interpreter (ld.so) at launch, before
 * a proc (and its fd table) exists. Returns 0 + sets data/len, or -ENOENT.
 */
long ove_lnx_rootfs_resolve(const ove_lnx_file_t *fs, int count, const char *abspath,
			    const uint8_t **data, size_t *len)
{
	int idx = fsx_follow(fs, count, fsx_lookup(fs, count, abspath));
	if (idx < 0)
		return -OVE_LNX_ENOENT;
	if (data)
		*data = fs[idx].data;
	if (len)
		*len = fs[idx].size;
	return 0;
}

/* ---- synthetic /proc (read-only, generated on open) ----------------------- */
static size_t p_str(char *o, size_t off, size_t cap, const char *s)
{
	while (*s && off < cap)
		o[off++] = *s++;
	return off;
}
static size_t p_dec(char *o, size_t off, size_t cap, uint64_t v)
{
	char t[20];
	int n = 0;
	if (!v)
		t[n++] = '0';
	while (v) {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	}
	while (n && off < cap)
		o[off++] = t[--n];
	return off;
}
#if defined(CONFIG_OVE_LINUX_NET)
/* Format a 4-byte IPv4 address as the 8 upper-hex digits the kernel writes in
 * /proc/net/route: the __be32 value read in the host's (little-endian) order. */
static size_t p_hexle(char *o, size_t off, size_t cap, const uint8_t a[4])
{
	static const char h[] = "0123456789ABCDEF";
	for (int i = 3; i >= 0; i--) {
		if (off < cap)
			o[off++] = h[a[i] >> 4];
		if (off < cap)
			o[off++] = h[a[i] & 0xf];
	}
	return off;
}
#endif

/* True for any path inside the synthetic /proc tree. */
static int proc_is(const char *abs)
{
	return strcmp(abs, "/proc") == 0 || strncmp(abs, "/proc/", 6) == 0;
}

/* Parse "/proc/<pid|self>[/file]": returns the pid (>0) + sets *file to the
 * trailing component (NULL if the path is the /proc/<pid> dir itself), or 0. */
static int proc_pid(const char *abs, const ove_lnx_proc_t *p, const char **file)
{
	*file = NULL;
	if (strncmp(abs, "/proc/", 6) != 0)
		return 0;
	const char *s = abs + 6;
	int pid = 0;
	if (strncmp(s, "self", 4) == 0 && (s[4] == '\0' || s[4] == '/')) {
		pid = p->pid;
		s += 4;
	} else if (*s >= '0' && *s <= '9') {
		while (*s >= '0' && *s <= '9')
			pid = pid * 10 + (*s++ - '0');
	} else {
		return 0;
	}
	if (*s == '/')
		*file = s + 1;
	else if (*s != '\0')
		return 0;
	return pid;
}

static int proc_pid_known(const ove_lnx_proc_t *p, int pid)
{
	/* pid 1 + the running process are always valid; every other live Linux slot
	 * and RTOS kernel thread comes from the ps/top snapshot. */
	return pid == 1 || pid == p->pid || ove_lnx_pent_find(pid) != NULL;
}

static const char *const g_proc_files[] = {"version", "uptime",	 "meminfo",	"cpuinfo", "mounts",
					   "stat",    "loadavg", "filesystems", NULL};

/* st_mode for a /proc node, or 0 if the path is not a synthetic /proc node. */
static uint32_t proc_mode(const char *abs, const ove_lnx_proc_t *p)
{
	if (strcmp(abs, "/proc") == 0)
		return OVE_LNX_S_IFDIR | 0555u;
	if (strcmp(abs, "/proc/self") == 0)
		return OVE_LNX_S_IFLNK | 0777u;
	const char *file;
	int pid = proc_pid(abs, p, &file);
	if (pid > 0)
		return !proc_pid_known(p, pid) ? 0u
		       : file		       ? (OVE_LNX_S_IFREG | 0444u)
					       : (OVE_LNX_S_IFDIR | 0555u);
#if defined(CONFIG_OVE_LINUX_NET)
	if (strcmp(abs, "/proc/net") == 0)
		return OVE_LNX_S_IFDIR | 0555u;
	if (strcmp(abs, "/proc/net/dev") == 0 || strcmp(abs, "/proc/net/route") == 0)
		return OVE_LNX_S_IFREG | 0444u;
#endif
	for (int i = 0; g_proc_files[i]; i++)
		if (strcmp(abs + 6, g_proc_files[i]) == 0)
			return OVE_LNX_S_IFREG | 0444u;
	return 0;
}

/* Generate the content of a /proc FILE into buf[cap]; returns length, or -1. */
static long proc_gen(const char *abs, const ove_lnx_proc_t *p, char *buf, size_t cap)
{
	size_t o = 0;
	const char *file;
	int pid = proc_pid(abs, p, &file);
	if (pid > 0 && file) {
		if (!proc_pid_known(p, pid))
			return -1;
		/* Metadata from the ps/top snapshot; fall back to pid 1 / the current
		 * process before the first snapshot refresh. comm is the bare name —
		 * kernel threads get an empty cmdline so ps/top bracket them as [name]. */
		const struct ove_lnx_pentry *e = ove_lnx_pent_find(pid);
		char comm[20];
		int ppid, is_kernel;
		char state;
		uint64_t cpu_us;
		size_t ci = 0;
		if (e) {
			for (const char *s = e->comm; *s && ci < sizeof(comm) - 1; s++)
				comm[ci++] = *s;
			ppid = e->ppid;
			state = e->state;
			cpu_us = e->cpu_us;
			is_kernel = e->is_kernel;
		} else {
			const char *c = (pid == 1)			? "init"
					: (pid == p->pid && p->comm[0]) ? p->comm
									: "busybox";
			for (const char *s = c; *s && ci < sizeof(comm) - 1; s++)
				comm[ci++] = *s;
			ppid = (pid == 1) ? 0 : (pid == p->pid) ? p->ppid : 1;
			state = (pid == p->pid) ? 'R' : 'S';
			cpu_us = ove_lnx_proc_cpu_us(pid);
			is_kernel = 0;
		}
		comm[ci] = '\0';
		uint64_t utime = cpu_us / 10000ull; /* USER_HZ = 100 → jiffies */
		if (strcmp(file, "stat") == 0) {
			o = p_dec(buf, o, cap, (uint64_t)pid);
			o = p_str(buf, o, cap, " (");
			o = p_str(buf, o, cap, comm);
			o = p_str(buf, o, cap, ") ");
			if (o < cap)
				buf[o++] = state;
			o = p_str(buf, o, cap, " ");
			o = p_dec(buf, o, cap, (uint64_t)ppid);
			/* fields 5..13 (pgrp..cmajflt), then field 14 utime, then 15..24. */
			o = p_str(buf, o, cap, " 0 0 0 0 0 0 0 0 0 ");
			o = p_dec(buf, o, cap, utime);
			o = p_str(buf, o, cap, " 0 0 0 0 0 0 0 0 0 0\n");
		} else if (strcmp(file, "cmdline") == 0) {
			/* kernel threads have a 0-byte cmdline so ps/top bracket them. */
			if (!is_kernel) {
				o = p_str(buf, o, cap, comm);
				if (o < cap)
					buf[o++] = '\0';
			}
		} else if (strcmp(file, "comm") == 0) {
			o = p_str(buf, o, cap, comm);
			o = p_str(buf, o, cap, "\n");
		} else if (strcmp(file, "status") == 0) {
			o = p_str(buf, o, cap, "Name:\t");
			o = p_str(buf, o, cap, comm);
			o = p_str(buf, o, cap, "\nState:\t");
			if (o < cap)
				buf[o++] = state;
			o = p_str(buf, o, cap,
				  (state == 'R') ? " (running)\nPid:\t" : " (sleeping)\nPid:\t");
			o = p_dec(buf, o, cap, (uint64_t)pid);
			o = p_str(buf, o, cap, "\nPPid:\t");
			o = p_dec(buf, o, cap, (uint64_t)ppid);
			o = p_str(buf, o, cap, "\n");
		} else {
			return -1;
		}
		return (long)o;
	}
	if (strcmp(abs, "/proc/version") == 0) {
		o = p_str(buf, o, cap, "Linux version 6.1.0 (overtos) (uClibc) #1 oveRTOS\n");
	} else if (strcmp(abs, "/proc/uptime") == 0) {
		uint64_t ns = 0;
		ove_time_get_ns(&ns);
		o = p_dec(buf, o, cap, ns / 1000000000ull);
		o = p_str(buf, o, cap, ".00 ");
		o = p_dec(buf, o, cap, ns / 1000000000ull);
		o = p_str(buf, o, cap, ".00\n");
	} else if (strcmp(abs, "/proc/meminfo") == 0) {
		o = p_str(buf, o, cap,
			  "MemTotal:       4096 kB\nMemFree:        2048 kB\n"
			  "MemAvailable:   2048 kB\nBuffers:           0 kB\nCached:            0 "
			  "kB\n");
	} else if (strcmp(abs, "/proc/cpuinfo") == 0) {
		o = p_str(buf, o, cap,
			  "processor\t: 0\nmodel name\t: ARM Cortex-M\nFeatures\t: thumb\n\n");
	} else if (strcmp(abs, "/proc/mounts") == 0) {
		o = p_str(buf, o, cap,
			  "rootfs / rootfs ro 0 0\nproc /proc proc rw 0 0\n"
			  "tmpfs /tmp tmpfs rw 0 0\n");
	} else if (strcmp(abs, "/proc/stat") == 0) {
		/* All busy time is reported as "user"; top derives %CPU from the
		 * user-vs-idle delta between two reads (USER_HZ = 100 → jiffies). */
		uint64_t idle_us = 0, busy_us = 0;
		ove_lnx_cpu_totals(&idle_us, &busy_us);
		uint64_t user = busy_us / 10000ull, idle = idle_us / 10000ull;
		o = p_str(buf, o, cap, "cpu  ");
		o = p_dec(buf, o, cap, user);
		o = p_str(buf, o, cap, " 0 0 ");
		o = p_dec(buf, o, cap, idle);
		o = p_str(buf, o, cap, " 0 0 0 0 0 0\ncpu0 ");
		o = p_dec(buf, o, cap, user);
		o = p_str(buf, o, cap, " 0 0 ");
		o = p_dec(buf, o, cap, idle);
		o = p_str(buf, o, cap, " 0 0 0 0 0 0\nctxt 0\nbtime 0\n");
	} else if (strcmp(abs, "/proc/loadavg") == 0) {
		int nproc = ove_lnx_pent_count();
		o = p_str(buf, o, cap, "0.00 0.00 0.00 1/");
		o = p_dec(buf, o, cap, (uint64_t)(nproc > 0 ? nproc : 1));
		o = p_str(buf, o, cap, " ");
		o = p_dec(buf, o, cap, (uint64_t)p->pid);
		o = p_str(buf, o, cap, "\n");
	} else if (strcmp(abs, "/proc/filesystems") == 0) {
		o = p_str(buf, o, cap, "nodev\tproc\nnodev\ttmpfs\n");
#if defined(CONFIG_OVE_LINUX_NET)
	} else if (strcmp(abs, "/proc/net/dev") == 0) {
		/* busybox ifconfig reads this to enumerate interfaces + show RX/TX stats.
		 * ove_net has no per-interface counters, so report zeros. */
		o = p_str(buf, o, cap,
			  "Inter-|   Receive                                                |  Transmit\n"
			  " face |bytes    packets errs drop fifo frame compressed multicast|bytes    "
			  "packets errs drop fifo colls carrier compressed\n");
		/* One interface (eth0). The SIOC* ioctls ignore ifr_name, so listing a
		 * loopback here would make busybox print it with eth0's data — omit it. */
		if (ove_lnx_sock_ifsnapshot(NULL, NULL, NULL, NULL, NULL) == 0)
			o = p_str(buf, o, cap,
				  "  eth0:       0       0    0    0    0     0          0         0"
				  "        0       0    0    0    0     0       0          0\n");
	} else if (strcmp(abs, "/proc/net/route") == 0) {
		uint8_t ip[4] = {0}, gw[4] = {0}, nm[4] = {0};
		o = p_str(buf, o, cap,
			  "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU"
			  "\tWindow\tIRTT\n");
		if (ove_lnx_sock_ifsnapshot(ip, gw, nm, NULL, NULL) == 0) {
			uint8_t net[4];
			for (int i = 0; i < 4; i++)
				net[i] = (uint8_t)(ip[i] & nm[i]);
			/* local subnet: dest = ip & mask, no gateway, flags = UP */
			o = p_str(buf, o, cap, "eth0\t");
			o = p_hexle(buf, o, cap, net);
			o = p_str(buf, o, cap, "\t00000000\t0001\t0\t0\t0\t");
			o = p_hexle(buf, o, cap, nm);
			o = p_str(buf, o, cap, "\t0\t0\t0\n");
			/* default route: dest = 0, gateway = gw, flags = UP|GATEWAY */
			if (gw[0] | gw[1] | gw[2] | gw[3]) {
				o = p_str(buf, o, cap, "eth0\t00000000\t");
				o = p_hexle(buf, o, cap, gw);
				o = p_str(buf, o, cap, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
			}
		}
#endif
	} else {
		return -1;
	}
	return (long)o;
}

/* Open a /proc node: a generated-content file fd, or a directory fd for
 * getdents. Returns an fd, or a negative errno (caller already resolved `abs`). */
static long proc_open(ove_lnx_proc_t *p, const char *abs)
{
	uint32_t m = proc_mode(abs, p);
	if (m == 0 || (m & OVE_LNX_S_IFMT) == OVE_LNX_S_IFLNK)
		return -OVE_LNX_ENOENT; /* /proc/self resolves via readlink, not open */
	int dir = (m & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR;
	for (int i = 0; i < OVE_LNX_NPROCF; i++) {
		if (g_procf[i].used)
			continue;
		long n = dir ? 0 : proc_gen(abs, p, g_procf[i].buf, OVE_LNX_PROCBUF);
		if (n < 0)
			return -OVE_LNX_ENOENT;
		strcpy(g_procf[i].path, abs);
		g_procf[i].len = (size_t)n;
		g_procf[i].is_dir = dir;
		g_procf[i].used = 1;
		return fd_alloc(p, OVE_LNX_FD_PROC, i, 0);
	}
	return -OVE_LNX_EMFILE;
}

static long sys_openat(ove_lnx_proc_t *p, int dirfd, const char *path, int flags)
{
	(void)dirfd; /* dirfd is AT_FDCWD; relative paths resolve against p->cwd */
	if (!path)
		return -OVE_LNX_EFAULT;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	path = abspath;
	if (proc_is(path)) /* synthetic /proc shadows everything */
		return proc_open(p, path);
	/* The console devices open as a read+write console (file_idx 2): getty opens
	 * /dev/console, makes it the controlling tty, and dups it to fds 0/1/2. */
	if (strcmp(path, "/dev/console") == 0 || strcmp(path, "/dev/tty") == 0 ||
	    strcmp(path, "/dev/tty0") == 0 || strcmp(path, "/dev/ttyS0") == 0)
		return fd_alloc(p, OVE_LNX_FD_CONSOLE, 2, 0);
	/* /dev/null (file_idx 3): reads EOF, writes are discarded. init points a
	 * child's stdio here when it has no controlling tty. */
	if (strcmp(path, "/dev/null") == 0)
		return fd_alloc(p, OVE_LNX_FD_CONSOLE, 3, 0);
#if defined(CONFIG_OVE_LINUX_DEV)
	/* Registered character devices (/dev/fb0, /dev/input/event0, ...). A hit opens
	 * an FD_DEV whose file_idx is the device open-pool index; a miss falls through. */
	{
		int di = ove_lnx_dev_lookup(path);
		if (di >= 0) {
			long oi = ove_lnx_dev_open_new(p, di, flags);
			if (oi < 0)
				return oi;
			int fd = fd_alloc(p, OVE_LNX_FD_DEV, (int)oi, 0);
			if (fd < 0)
				ove_lnx_dev_close((int)oi);
			return fd;
		}
	}
#endif
	int wr = (flags & OVE_LNX_O_ACCMODE) != OVE_LNX_O_RDONLY;
	int wi = wfs_find(path);

	/* A writable open (or O_CREAT) goes to the writable VFS overlay. */
	if (wr || (flags & OVE_LNX_O_CREAT)) {
		if (wi < 0) {
			if (!(flags & OVE_LNX_O_CREAT)) {
				if (fs_lookup(p, path) >= 0)
					return -OVE_LNX_EROFS; /* RO rootfs file */
				return -OVE_LNX_ENOENT;
			}
			wi = wfs_create(path, OVE_LNX_S_IFREG | 0644u);
			if (wi < 0)
				return -OVE_LNX_EMFILE;
		} else {
			if ((g_wnodes[wi].mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
				return -OVE_LNX_EISDIR;
			if (flags & OVE_LNX_O_TRUNC)
				g_wnodes[wi].size = 0;
		}
		return fd_alloc(p, OVE_LNX_FD_TMPFS, wi,
				(flags & OVE_LNX_O_APPEND) ? g_wnodes[wi].size : 0);
	}

	/* Read: a writable node shadows the rootfs; else the read-only rootfs. */
	if (wi >= 0)
		return fd_alloc(p, OVE_LNX_FD_TMPFS, wi, 0);
	/* Follow symlinks so a read open of e.g. /lib/libc.so.0 -> libuClibc.so returns the
	 * target ELF (ld.so opens its .so deps by their symlinked SONAMEs). */
	int idx = fs_follow(p, fs_lookup(p, path));
	if (idx >= 0)
		return fd_alloc(p, OVE_LNX_FD_FILE, idx, 0);
	return -OVE_LNX_ENOENT;
}

static long sys_close(ove_lnx_proc_t *p, int fd)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (s->kind == OVE_LNX_FD_PROC)
		g_procf[s->file_idx].used = 0; /* release the generated-content slot */
#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		ove_lnx_dev_close(s->file_idx); /* refs--, ops->release at the last close */
#endif
#if defined(CONFIG_OVE_LINUX_NET)
	if (s->kind == OVE_LNX_FD_SOCKET)
		ove_lnx_sock_close(s->file_idx); /* refs--, ove_socket_close at the last close */
#endif
	s->kind = OVE_LNX_FD_FREE;
	return 0;
}

/* pipe(2): allocate a pipe object + a read-end / write-end fd pair. */
static long sys_pipe(ove_lnx_proc_t *p, int *fds)
{
	if (!user_ok(p, fds, 2 * sizeof(int), 1)) /* the kernel writes fds[0],fds[1] */
		return -OVE_LNX_EFAULT;
	/* A pipe slot is free when no live proc holds either end (auto-reclaimed when
	 * both ends close or the holders exit — there is no explicit pipe free path). */
	int pi = -1;
	for (int i = 0; i < OVE_LNX_NPIPE; i++) {
		int rd, wr;
		pipe_ends(i, &rd, &wr);
		if (rd == 0 && wr == 0) {
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
	g_pipes[pi].count = 0;
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
	if (oldfd != newfd) {
#if defined(CONFIG_OVE_LINUX_DEV)
		if (p->fds[newfd].kind == OVE_LNX_FD_DEV)
			ove_lnx_dev_close(p->fds[newfd].file_idx); /* dup2 closes the target first */
#endif
#if defined(CONFIG_OVE_LINUX_NET)
		if (p->fds[newfd].kind == OVE_LNX_FD_SOCKET)
			ove_lnx_sock_close(p->fds[newfd].file_idx); /* dup2 closes the target first */
#endif
		p->fds[newfd] = *s;
#if defined(CONFIG_OVE_LINUX_DEV)
		if (s->kind == OVE_LNX_FD_DEV)
			ove_lnx_dev_get(s->file_idx); /* the new fd shares the open */
#endif
#if defined(CONFIG_OVE_LINUX_NET)
		if (s->kind == OVE_LNX_FD_SOCKET)
			ove_lnx_sock_get(s->file_idx); /* the new fd shares the open */
#endif
	}
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
#if defined(CONFIG_OVE_LINUX_DEV)
			if (s->kind == OVE_LNX_FD_DEV)
				ove_lnx_dev_get(s->file_idx); /* the dup shares the open */
#endif
#if defined(CONFIG_OVE_LINUX_NET)
			if (s->kind == OVE_LNX_FD_SOCKET)
				ove_lnx_sock_get(s->file_idx); /* the dup shares the open */
#endif
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
#if defined(CONFIG_OVE_LINUX_DEV)
	if (s->kind == OVE_LNX_FD_DEV)
		return ove_lnx_dev_lseek(s->file_idx, off, whence);
#endif
	if (s->kind != OVE_LNX_FD_FILE && s->kind != OVE_LNX_FD_TMPFS)
		return -OVE_LNX_ESPIPE; /* console/pipe is not seekable */

	long end = (s->kind == OVE_LNX_FD_TMPFS) ? (long)g_wnodes[s->file_idx].size
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

/* _llseek(fd, offset_high, offset_low, loff_t *result, whence): the 64-bit-offset
 * seek uClibc uses in large-file mode. Pagers/editors (less/more/vi) seek to size
 * the file (the %-position). Our files are well under 4 GB so offset_high is 0. */
static long sys_llseek(ove_lnx_proc_t *p, int fd, unsigned long off_hi, unsigned long off_lo,
		       uint64_t *result, unsigned int whence)
{
	(void)off_hi;
	long pos = sys_lseek(p, fd, (long)off_lo, (int)whence);
	if (pos < 0)
		return pos;
	if (result && !user_ok(p, result, sizeof(*result), 1))
		return -OVE_LNX_EFAULT;
	if (result)
		*result = (uint64_t)pos;
	return 0;
}

/* ftruncate64(fd, length) on a writable-VFS file: set its logical size, growing
 * with zeros if needed. vi's :w writes the new content then truncates to the exact
 * length, so editing an existing file shorter drops the old trailing bytes. */
static long sys_ftruncate(ove_lnx_proc_t *p, int fd, uint64_t length)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (s->kind != OVE_LNX_FD_TMPFS)
		return -OVE_LNX_EINVAL; /* the rootfs is read-only; console/pipe N/A */
	ove_lnx_wnode_t *t = &g_wnodes[s->file_idx];
	if ((t->mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
		return -OVE_LNX_EISDIR;
	size_t newlen = (size_t)length;
	if (newlen > t->size) {
		if (wfs_reserve(s->file_idx, newlen) != 0)
			return -OVE_LNX_EFBIG;
		memset(t->data + t->size, 0, newlen - t->size);
	}
	t->size = newlen;
	return 0;
}

/* Fill an ARM kstat64 from a node's inode + mode + size. */
static void fill_kstat64(struct ove_lnx_kstat64 *st, uint32_t ino, uint32_t mode, uint64_t size)
{
	memset(st, 0, sizeof(*st));
	st->st_nlink = 1;
	/* A UNIQUE, non-zero inode per node: ld.so dedups loaded objects by (st_dev, st_ino),
	 * so a zero inode makes every .so look already-loaded — libc.so would be skipped and
	 * its symbols never resolve. */
	st->__st_ino = ino;
	st->st_ino = ino;
	st->st_mode = mode;
	st->st_size = (int64_t)size;
	/* A character device blksize makes uClibc block-buffer stdio. */
	st->st_blksize = ((mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFCHR) ? 1024u : 512u;
	st->st_blocks = (uint64_t)((size + 511u) / 512u);
}

static long sys_fstat64(ove_lnx_proc_t *p, int fd, void *statbuf)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (!user_ok(p, statbuf, sizeof(struct ove_lnx_kstat64), 1))
		return -OVE_LNX_EFAULT;
	if (s->kind == OVE_LNX_FD_FILE)
		fill_kstat64(statbuf, 1u + (uint32_t)s->file_idx,
			     file_mode(&p->fs[s->file_idx]), p->fs[s->file_idx].size);
	else if (s->kind == OVE_LNX_FD_TMPFS)
		fill_kstat64(statbuf, 0x100000u + (uint32_t)s->file_idx,
			     g_wnodes[s->file_idx].mode, g_wnodes[s->file_idx].size);
	else if (s->kind == OVE_LNX_FD_PROC)
		fill_kstat64(statbuf, 0x200000u + (uint32_t)s->file_idx,
			     g_procf[s->file_idx].is_dir ? (OVE_LNX_S_IFDIR | 0555u)
							 : (OVE_LNX_S_IFREG | 0444u),
			     g_procf[s->file_idx].len);
#if defined(CONFIG_OVE_LINUX_DEV)
	else if (s->kind == OVE_LNX_FD_DEV) {
		uint32_t mode;
		uint64_t rdev, size;
		ove_lnx_dev_fstat(s->file_idx, &mode, &rdev, &size);
		fill_kstat64(statbuf, 0x300000u + (uint32_t)s->file_idx, mode, size);
		((struct ove_lnx_kstat64 *)statbuf)->st_rdev = rdev;
	}
#endif
#if defined(CONFIG_OVE_LINUX_NET)
	else if (s->kind == OVE_LNX_FD_SOCKET) {
		uint32_t mode;
		uint64_t size;
		ove_lnx_sock_fstat(s->file_idx, &mode, &size);
		fill_kstat64(statbuf, 0x400000u + (uint32_t)s->file_idx, mode, size);
	}
#endif
	else
		fill_kstat64(statbuf, 0x300000u + (uint32_t)s->file_idx, OVE_LNX_S_IFCHR | 0620u, 0);
	return 0;
}

/* path-based stat: resolve, optionally follow a trailing symlink, fill kstat64. */
static long sys_stat_path(ove_lnx_proc_t *p, const char *path, int follow, void *statbuf)
{
	if (!user_ok(p, statbuf, sizeof(struct ove_lnx_kstat64), 1))
		return -OVE_LNX_EFAULT; /* path is validated by resolve_path below */
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	if (proc_is(abspath)) {
		uint32_t m = proc_mode(abspath, p);
		if (m == 0)
			return -OVE_LNX_ENOENT;
		fill_kstat64(statbuf, 0x200000u, m, 0);
		return 0;
	}
#if defined(CONFIG_OVE_LINUX_DEV)
	{
		uint32_t dmode;
		uint64_t drdev;
		if (ove_lnx_dev_stat_path(abspath, &dmode, &drdev) == 0) {
			fill_kstat64(statbuf, 0x300000u, dmode, 0);
			((struct ove_lnx_kstat64 *)statbuf)->st_rdev = drdev;
			return 0;
		}
	}
#endif
	int wi = wfs_find(abspath); /* writable overlay shadows the rootfs */
	if (wi >= 0) {
		fill_kstat64(statbuf, 0x100000u + (uint32_t)wi, g_wnodes[wi].mode, g_wnodes[wi].size);
		return 0;
	}
	int idx = fs_lookup(p, abspath);
	if (idx < 0)
		return -OVE_LNX_ENOENT;
	if (follow) {
		idx = fs_follow(p, idx);
		if (idx < 0)
			return -OVE_LNX_ENOENT;
	}
	fill_kstat64(statbuf, 1u + (uint32_t)idx, file_mode(&p->fs[idx]), p->fs[idx].size);
	return 0;
}

/* readlink: write the symlink target (not NUL-terminated) + return its length. */
static long sys_readlink(ove_lnx_proc_t *p, const char *path, char *buf, size_t bufsiz)
{
	if (!user_ok(p, buf, bufsiz, 1))
		return -OVE_LNX_EFAULT; /* path is validated by resolve_path below */
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	if (strcmp(abspath, "/proc/self") == 0) { /* -> the running process's pid */
		char tmp[12];
		size_t n = p_dec(tmp, 0, sizeof(tmp), (uint64_t)p->pid);
		if (n > bufsiz)
			n = bufsiz;
		memcpy(buf, tmp, n);
		return (long)n;
	}
	int wi = wfs_find(abspath); /* a writable symlink (ln -s) shadows the rootfs */
	if (wi >= 0) {
		ove_lnx_wnode_t *w = &g_wnodes[wi];
		if ((w->mode & OVE_LNX_S_IFMT) != OVE_LNX_S_IFLNK || !w->data)
			return -OVE_LNX_EINVAL;
		size_t n = w->size > bufsiz ? bufsiz : w->size;
		memcpy(buf, w->data, n);
		return (long)n;
	}
	int idx = fs_lookup(p, abspath);
	if (idx < 0)
		return -OVE_LNX_ENOENT;
	const ove_lnx_file_t *lnk = &p->fs[idx];
	if ((file_mode(lnk) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFLNK || !lnk->data)
		return -OVE_LNX_EINVAL;
	size_t n = lnk->size > bufsiz ? bufsiz : lnk->size;
	memcpy(buf, lnk->data, n);
	return (long)n;
}

/* access/faccessat: existence check (all existing nodes are accessible). */
static long sys_access(ove_lnx_proc_t *p, const char *path)
{
	if (!path)
		return -OVE_LNX_EFAULT;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	if (abspath[0] == '/' && abspath[1] == '\0')
		return 0; /* root */
	if (proc_is(abspath))
		return proc_mode(abspath, p) ? 0 : -OVE_LNX_ENOENT;
	if (wfs_find(abspath) >= 0 || fs_lookup(p, abspath) >= 0)
		return 0;
	return -OVE_LNX_ENOENT;
}

static long sys_mkdir(ove_lnx_proc_t *p, const char *path, uint32_t mode)
{
	if (!path)
		return -OVE_LNX_EFAULT;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	if (wfs_find(abspath) >= 0 || fs_lookup(p, abspath) >= 0)
		return -OVE_LNX_EEXIST;
	if (wfs_create(abspath, OVE_LNX_S_IFDIR | (mode & 0777u)) < 0)
		return -OVE_LNX_ENOSPC;
	return 0;
}

/* unlink (is_rmdir=0) / rmdir (is_rmdir=1) on a writable node. */
static long sys_unlink(ove_lnx_proc_t *p, const char *path, int is_rmdir)
{
	if (!path)
		return -OVE_LNX_EFAULT;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	int wi = wfs_find(abspath);
	if (wi < 0)
		return (fs_lookup(p, abspath) >= 0) ? -OVE_LNX_EROFS : -OVE_LNX_ENOENT;
	int isdir = (g_wnodes[wi].mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR;
	if (is_rmdir && !isdir)
		return -OVE_LNX_ENOTDIR;
	if (!is_rmdir && isdir)
		return -OVE_LNX_EISDIR;
	if (isdir) {
		for (int j = 0; j < OVE_LNX_NWNODE; j++)
			if (g_wnodes[j].used && child_name(abspath, g_wnodes[j].path))
				return -OVE_LNX_ENOTEMPTY;
	}
	g_wnodes[wi].used = 0; /* node freed; its pool bytes leak (bounded) */
	return 0;
}

static long sys_rename(ove_lnx_proc_t *p, const char *oldp, const char *newp)
{
	if (!oldp || !newp)
		return -OVE_LNX_EFAULT;
	char oldabs[OVE_LNX_PATH_MAX], newabs[OVE_LNX_PATH_MAX];
	long r1 = resolve_path(p, oldp, oldabs, sizeof(oldabs));
	if (r1 < 0)
		return r1;
	long r2 = resolve_path(p, newp, newabs, sizeof(newabs));
	if (r2 < 0)
		return r2;
	int wi = wfs_find(oldabs);
	if (wi < 0)
		return (fs_lookup(p, oldabs) >= 0) ? -OVE_LNX_EROFS : -OVE_LNX_ENOENT;
	if (strlen(newabs) >= OVE_LNX_PATH_MAX)
		return -OVE_LNX_ENAMETOOLONG;
	int di = wfs_find(newabs); /* replace an existing destination node */
	if (di >= 0 && di != wi)
		g_wnodes[di].used = 0;
	strcpy(g_wnodes[wi].path, newabs);
	return 0;
}

static long sys_symlink(ove_lnx_proc_t *p, const char *target, const char *linkp)
{
	if (!target || !linkp)
		return -OVE_LNX_EFAULT;
	char linkabs[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, linkp, linkabs, sizeof(linkabs));
	if (rr < 0)
		return rr;
	if (wfs_find(linkabs) >= 0 || fs_lookup(p, linkabs) >= 0)
		return -OVE_LNX_EEXIST;
	int wi = wfs_create(linkabs, OVE_LNX_S_IFLNK | 0777u);
	if (wi < 0)
		return -OVE_LNX_ENOSPC;
	size_t tl = strlen(target);
	if (wfs_reserve(wi, tl) < 0) {
		g_wnodes[wi].used = 0;
		return -OVE_LNX_ENOSPC;
	}
	memcpy(g_wnodes[wi].data, target, tl);
	g_wnodes[wi].size = tl;
	return 0;
}

static long sys_chmod(ove_lnx_proc_t *p, const char *path, uint32_t mode)
{
	if (!path)
		return -OVE_LNX_EFAULT;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	int wi = wfs_find(abspath);
	if (wi >= 0) {
		g_wnodes[wi].mode = (g_wnodes[wi].mode & OVE_LNX_S_IFMT) | (mode & 0777u);
		return 0;
	}
	return (fs_lookup(p, abspath) >= 0) ? 0 : -OVE_LNX_ENOENT; /* rootfs: accept, inert */
}

/* utimensat: times are not tracked, but the existence check must be honest —
 * `touch` probes with utimensat first and only creates the file on -ENOENT. */
static long sys_utimensat(ove_lnx_proc_t *p, const char *path)
{
	if (!path) /* futimens(fd): operate on the open fd — accept */
		return 0;
	char abspath[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, abspath, sizeof(abspath));
	if (rr < 0)
		return rr;
	if ((abspath[0] == '/' && abspath[1] == '\0') || wfs_find(abspath) >= 0 ||
	    fs_lookup(p, abspath) >= 0)
		return 0;
	return -OVE_LNX_ENOENT;
}

/* getrandom: a non-cryptographic xorshift PRNG seeded from uptime (no hardware
 * RNG); enough for mktemp suffixes etc. */
static long sys_getrandom(ove_lnx_proc_t *p, void *buf, size_t count)
{
	if (!user_ok(p, buf, count, 1))
		return -OVE_LNX_EFAULT;
	static uint32_t s;
	if (!s) {
		uint64_t ns = 0;
		ove_time_get_ns(&ns);
		s = (uint32_t)ns | 1u;
	}
	uint8_t *b = buf;
	for (size_t i = 0; i < count; i++) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		b[i] = (uint8_t)(s >> 24);
	}
	return (long)count;
}

/* statfs64: synthetic filesystem stats (no real block device backs the rootfs). */
struct ove_lnx_statfs64 {
	uint32_t f_type, f_bsize;
	uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	uint32_t f_fsid[2], f_namelen, f_frsize, f_flags, f_spare[4];
};
static long sys_statfs(ove_lnx_proc_t *p, void *buf)
{
	if (!user_ok(p, buf, sizeof(struct ove_lnx_statfs64), 1))
		return -OVE_LNX_EFAULT;
	struct ove_lnx_statfs64 *st = buf;
	memset(st, 0, sizeof(*st));
	st->f_type = 0x01021994u; /* TMPFS_MAGIC */
	st->f_bsize = 4096;
	st->f_frsize = 4096;
	st->f_blocks = 256;
	st->f_bfree = 192;
	st->f_bavail = 192;
	st->f_files = 64;
	st->f_ffree = 48;
	st->f_namelen = 255;
	return 0;
}

/* Append one dirent record. Skips entries already emitted (pos < s->offset);
 * returns 0 if the record does not fit (caller stops), else 1 (and advances). */
static int dirent_emit(uint8_t *out, size_t count, size_t *filled, long *pos, ove_lnx_fd_t *s,
		       uint64_t ino, const char *name, uint32_t mode)
{
	if (*pos < (long)s->offset) {
		(*pos)++;
		return 1; /* already returned by an earlier getdents call */
	}
	size_t namelen = strlen(name);
	size_t reclen = (offsetof(struct ove_lnx_dirent64, d_name) + namelen + 1 + 7u) &
			~(size_t)7u;
	if (*filled + reclen > count)
		return 0;
	struct ove_lnx_dirent64 *de = (struct ove_lnx_dirent64 *)(out + *filled);
	de->d_ino = ino;
	de->d_off = *pos + 1;
	de->d_reclen = (uint16_t)reclen;
	de->d_type = ((mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)   ? OVE_LNX_DT_DIR
		     : ((mode & OVE_LNX_S_IFMT) == OVE_LNX_S_IFCHR) ? OVE_LNX_DT_CHR
								    : OVE_LNX_DT_REG;
	memcpy(de->d_name, name, namelen + 1);
	*filled += reclen;
	(*pos)++;
	s->offset++;
	return 1;
}

/* getdents64: emit the directory's immediate children (read-only rootfs entries
 * + writable-overlay nodes) as linux_dirent64 records. */
static long sys_getdents64(ove_lnx_proc_t *p, int fd, void *buf, size_t count)
{
	ove_lnx_fd_t *s = fd_slot(p, fd);
	if (!s)
		return -OVE_LNX_EBADF;
	if (!user_ok(p, buf, count, 1))
		return -OVE_LNX_EFAULT;
	const char *dirpath;
	if (s->kind == OVE_LNX_FD_FILE) {
		if ((file_mode(&p->fs[s->file_idx]) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFDIR)
			return -OVE_LNX_ENOTDIR;
		dirpath = p->fs[s->file_idx].path;
	} else if (s->kind == OVE_LNX_FD_TMPFS) {
		if ((g_wnodes[s->file_idx].mode & OVE_LNX_S_IFMT) != OVE_LNX_S_IFDIR)
			return -OVE_LNX_ENOTDIR;
		dirpath = g_wnodes[s->file_idx].path;
	} else if (s->kind == OVE_LNX_FD_PROC) {
		if (!g_procf[s->file_idx].is_dir)
			return -OVE_LNX_ENOTDIR;
		dirpath = g_procf[s->file_idx].path;
	} else {
		return -OVE_LNX_ENOTDIR;
	}

	uint8_t *out = (uint8_t *)buf;
	size_t filled = 0;
	long pos = 0; /* running child index across both sources; s->offset = emitted */
	int full = 0;
	/* rootfs children (a writable node of the same path shadows the rootfs one) */
	for (int i = 0; i < p->fs_count && !full; i++) {
		const char *name = child_name(dirpath, p->fs[i].path);
		if (!name || wfs_find(p->fs[i].path) >= 0)
			continue;
		if (!dirent_emit(out, count, &filled, &pos, s, (uint64_t)(i + 1), name,
				 file_mode(&p->fs[i])))
			full = 1;
	}
	/* writable-overlay children */
	for (int i = 0; i < OVE_LNX_NWNODE && !full; i++) {
		if (!g_wnodes[i].used)
			continue;
		const char *name = child_name(dirpath, g_wnodes[i].path);
		if (!name)
			continue;
		if (!dirent_emit(out, count, &filled, &pos, s, (uint64_t)(100000 + i), name,
				 g_wnodes[i].mode))
			full = 1;
	}
#if defined(CONFIG_OVE_LINUX_DEV)
	/* registered character devices whose node sits directly under this dir (/dev/fb0). */
	for (int i = 0; i < ove_lnx_dev_count() && !full; i++) {
		uint32_t dmode = OVE_LNX_S_IFCHR | 0666u;
		const char *dp = ove_lnx_dev_path(i, &dmode);
		const char *name = dp ? child_name(dirpath, dp) : NULL;
		if (!name)
			continue;
		if (!dirent_emit(out, count, &filled, &pos, s, (uint64_t)(0x300000 + i), name, dmode))
			full = 1;
	}
#endif
	/* synthetic /proc children */
	if (!full && proc_is(dirpath)) {
		const char *file;
		int dpid = proc_pid(dirpath, p, &file);
		if (strcmp(dirpath, "/proc") == 0) {
			uint64_t ino = 200000;
			for (int i = 0; g_proc_files[i] && !full; i++)
				if (!dirent_emit(out, count, &filled, &pos, s, ino++,
						 g_proc_files[i], OVE_LNX_S_IFREG))
					full = 1;
			if (!full && !dirent_emit(out, count, &filled, &pos, s, ino++, "self",
						  OVE_LNX_S_IFLNK))
				full = 1;
			/* every live process + kernel thread from the ps/top snapshot */
			int np = ove_lnx_pent_count(), seen1 = 0, seenself = 0;
			for (int i = 0; i < np && !full; i++) {
				const struct ove_lnx_pentry *e = ove_lnx_pent_at(i);
				if (!e)
					break;
				char pidstr[12];
				size_t k = p_dec(pidstr, 0, sizeof(pidstr) - 1, (uint64_t)e->pid);
				pidstr[k] = '\0';
				seen1 |= (e->pid == 1);
				seenself |= (e->pid == p->pid);
				if (!dirent_emit(out, count, &filled, &pos, s, ino++, pidstr,
						 OVE_LNX_S_IFDIR))
					full = 1;
			}
			/* fallbacks before the first snapshot refresh populates the table */
			if (!full && !seen1 &&
			    !dirent_emit(out, count, &filled, &pos, s, ino++, "1", OVE_LNX_S_IFDIR))
				full = 1;
			if (!full && !seenself && p->pid != 1) {
				char pidstr[12];
				size_t k = p_dec(pidstr, 0, sizeof(pidstr) - 1, (uint64_t)p->pid);
				pidstr[k] = '\0';
				if (!dirent_emit(out, count, &filled, &pos, s, ino++, pidstr,
						 OVE_LNX_S_IFDIR))
					full = 1;
			}
		} else if (dpid > 0 && !file && proc_pid_known(p, dpid)) {
			static const char *const pf[] = {"stat", "cmdline", "status", "comm", NULL};
			uint64_t ino = 300000;
			for (int i = 0; pf[i] && !full; i++)
				if (!dirent_emit(out, count, &filled, &pos, s, ino++, pf[i],
						 OVE_LNX_S_IFREG))
					full = 1;
		}
	}
	if (full && filled == 0)
		return -OVE_LNX_EINVAL; /* buffer too small for even one entry */
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
	uint8_t __times[64];	 /* atime/btime/ctime/mtime (4 x 16B) — offsets 64..128 */
	uint32_t stx_rdev_major; /* offset 128 */
	uint32_t stx_rdev_minor; /* offset 132 */
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
	uint8_t __rest[256 - 144];
};

/*
 * statx: the stat() uClibc-ng actually issues. With AT_EMPTY_PATH (or an empty
 * path) it stats the open dirfd (fstat); otherwise it resolves a rootfs path.
 */
static long sys_statx(ove_lnx_proc_t *p, int dirfd, const char *path, int flags, void *buf)
{
	if (!user_ok(p, buf, sizeof(struct ove_lnx_statx), 1))
		return -OVE_LNX_EFAULT;

	uint32_t mode;
	uint64_t size;
	uint64_t rdev = 0;	  /* device id for a character node, else 0 */
	uint32_t ino = 0x300000u; /* unique, non-zero inode: ld.so dedups by (st_dev, st_ino) */
	if (path && path[0] && !(flags & OVE_LNX_AT_EMPTY_PATH)) {
		char abspath[OVE_LNX_PATH_MAX];
		long rr = resolve_path(p, path, abspath, sizeof(abspath));
		if (rr < 0)
			return rr;
		int wi;
		if (proc_is(abspath)) {
			mode = proc_mode(abspath, p);
			if (mode == 0)
				return -OVE_LNX_ENOENT;
			size = 0;
			ino = 0x200000u;
#if defined(CONFIG_OVE_LINUX_DEV)
		} else if (ove_lnx_dev_lookup(abspath) >= 0) { /* /dev character node */
			uint32_t dmode;
			uint64_t drdev;
			ove_lnx_dev_stat_path(abspath, &dmode, &drdev);
			mode = dmode;
			size = 0;
			rdev = drdev;
			ino = 0x300000u;
#endif
		} else if ((wi = wfs_find(abspath)) >= 0) { /* writable overlay shadows rootfs */
			mode = g_wnodes[wi].mode;
			size = g_wnodes[wi].size;
			ino = 0x100000u + (uint32_t)wi;
		} else {
			int idx = fs_lookup(p, abspath);
			if (idx < 0)
				return -OVE_LNX_ENOENT;
			if (!(flags & OVE_LNX_AT_SYMLINK_NOFOLLOW)) { /* lstat passes NOFOLLOW */
				idx = fs_follow(p, idx);
				if (idx < 0)
					return -OVE_LNX_ENOENT;
			}
			mode = file_mode(&p->fs[idx]);
			size = p->fs[idx].size;
			ino = 1u + (uint32_t)idx;
		}
	} else {
		ove_lnx_fd_t *s = fd_slot(p, dirfd);
		if (!s)
			return -OVE_LNX_EBADF;
		if (s->kind == OVE_LNX_FD_FILE) {
			mode = file_mode(&p->fs[s->file_idx]);
			size = p->fs[s->file_idx].size;
			ino = 1u + (uint32_t)s->file_idx;
		} else if (s->kind == OVE_LNX_FD_TMPFS) {
			mode = g_wnodes[s->file_idx].mode;
			size = g_wnodes[s->file_idx].size;
			ino = 0x100000u + (uint32_t)s->file_idx;
#if defined(CONFIG_OVE_LINUX_DEV)
		} else if (s->kind == OVE_LNX_FD_DEV) {
			uint32_t dmode;
			uint64_t drdev, dsize;
			ove_lnx_dev_fstat(s->file_idx, &dmode, &drdev, &dsize);
			mode = dmode;
			size = dsize;
			rdev = drdev;
			ino = 0x300000u + (uint32_t)s->file_idx;
#endif
		} else {
			mode = OVE_LNX_S_IFCHR | 0620u;
			size = 0;
			ino = 0x300000u + (uint32_t)s->file_idx;
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
	st->stx_ino = ino; /* ld.so dedups loaded .so objects by (st_dev, st_ino) */
	st->stx_rdev_major = (uint32_t)(rdev >> 8);
	st->stx_rdev_minor = (uint32_t)(rdev & 0xffu);
	return 0;
}

/*
 * execve: resolve the program in the rootfs and capture its argument vector,
 * then flag the request. The per-engine seam (privileged) does the actual image
 * replacement — reload the bFLT, rebuild the MPU domain + stack, and relaunch
 * the thread — because that is engine-specific. We never truly return: on
 * success the old image is gone; on failure we report a negated errno.
 */
/* Append one NUL-terminated arg to the pending-exec argv; ignore on overflow. */
static void exec_push_arg(ove_lnx_proc_t *p, int *argc, size_t *off, const char *str)
{
	if (*argc >= OVE_LNX_EXEC_MAXARGS)
		return;
	size_t n = strlen(str) + 1;
	if (*off + n > sizeof(p->exec_argv_buf))
		return;
	memcpy(p->exec_argv_buf + *off, str, n);
	p->exec_argv[*argc] = p->exec_argv_buf + *off;
	*off += n;
	(*argc)++;
}

static long sys_execve(ove_lnx_proc_t *p, const char *path, char *const argv[])
{
	if (!path)
		return -OVE_LNX_EFAULT;
	/* Validate the whole argv vector before the walk below reads it: a malicious argv could point at
	 * kernel memory or never NUL-terminate. Each element must be readable and each string in-bounds;
	 * cap the count so a non-terminated array can't spin. */
	if (argv) {
		for (int j = 0;; j++) {
			if (j > 256)
				return -OVE_LNX_EINVAL;
			if (!user_ok(p, &argv[j], sizeof(argv[j]), 0))
				return -OVE_LNX_EFAULT;
			if (!argv[j])
				break;
			if (user_strnlen(p, argv[j], (size_t)-1) < 0)
				return -OVE_LNX_EFAULT;
		}
	}
	char execabs[OVE_LNX_PATH_MAX];
	long rr = resolve_path(p, path, execabs, sizeof(execabs));
	if (rr < 0)
		return rr;
	/* Follow symlinks, e.g. /bin/echo -> busybox (Buildroot installs applets as
	 * symlinks). The argv (argv[0] = "echo") is kept, so busybox runs that applet. */
	int idx = fs_follow(p, fs_lookup(p, execabs));
	if (idx < 0)
		return -OVE_LNX_ENOENT;
	if ((file_mode(&p->fs[idx]) & OVE_LNX_S_IFMT) == OVE_LNX_S_IFDIR)
		return -OVE_LNX_EACCES;

	/* Interpreter scripts: a "#!interp [arg]" first line re-targets the exec to
	 * the interpreter, with argv = [interp, arg?, scriptpath, original argv[1:]].
	 * init runs /etc/init.d/rcS (a #!/bin/sh script) this way. */
	const ove_lnx_file_t *f = &p->fs[idx];
	char interp[64], iarg[64];
	int have_iarg = 0, interp_idx = -1;
	if (f->data && f->size >= 2 && f->data[0] == '#' && f->data[1] == '!') {
		const char *s = (const char *)f->data + 2, *end = (const char *)f->data + f->size;
		while (s < end && (*s == ' ' || *s == '\t'))
			s++;
		int k = 0;
		while (s < end && *s != ' ' && *s != '\t' && *s != '\n' &&
		       k < (int)sizeof(interp) - 1)
			interp[k++] = *s++;
		interp[k] = '\0';
		while (s < end && (*s == ' ' || *s == '\t'))
			s++;
		int m = 0;
		while (s < end && *s != '\n' && *s != ' ' && *s != '\t' &&
		       m < (int)sizeof(iarg) - 1)
			iarg[m++] = *s++;
		iarg[m] = '\0';
		have_iarg = (m > 0);
		if (k == 0)
			return -OVE_LNX_ENOEXEC;
		char interpabs[OVE_LNX_PATH_MAX];
		if (resolve_path(p, interp, interpabs, sizeof(interpabs)) < 0)
			return -OVE_LNX_ENOENT;
		interp_idx = fs_follow(p, fs_lookup(p, interpabs));
		if (interp_idx < 0)
			return -OVE_LNX_ENOENT;
	}

	int argc = 0;
	size_t off = 0;
	if (interp_idx >= 0) {
		exec_push_arg(p, &argc, &off, interp);
		if (have_iarg)
			exec_push_arg(p, &argc, &off, iarg);
		exec_push_arg(p, &argc, &off, execabs); /* the script pathname */
		for (int j = 1; argv && argv[j]; j++)
			exec_push_arg(p, &argc, &off, argv[j]);
		idx = interp_idx;
	} else {
		for (int j = 0; argv && argv[j]; j++)
			exec_push_arg(p, &argc, &off, argv[j]);
	}
	p->exec_argc = argc;
	p->exec_file_idx = idx;
	p->exec_pending = 1;
	return 0;
}

/* There is no RTC: wall-clock time is a fixed base epoch (~2026-06-23) + uptime. */
#define OVE_LNX_BOOT_EPOCH 1782172800ull

static void now_sec_nsec(int clockid, uint64_t *sec, uint32_t *nsec)
{
	uint64_t ns = 0;
	ove_time_get_ns(&ns);
	uint64_t up = ns / 1000000000ull;
	*nsec = (uint32_t)(ns % 1000000000ull);
	/* CLOCK_MONOTONIC(1)/_RAW(4)/BOOTTIME(7) → uptime; REALTIME(0) → wall clock. */
	*sec = (clockid == 0) ? (OVE_LNX_BOOT_EPOCH + up) : up;
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
		return sys_mmap2(proc, (uintptr_t)a0, (size_t)a1, (int)a2, (int)a3, (int)a4,
				 (uint32_t)a5);
	case OVE_LNX_NR_munmap:
		return sys_munmap(proc, (uintptr_t)a0, (size_t)a1);
	case OVE_LNX_NR_mprotect: /* NOMMU: RELRO/protection is a no-op */
		return sys_mprotect((uintptr_t)a0, (size_t)a1, (int)a2);
	case OVE_LNX_NR_pread64: /* (fd, buf, count, [pad a3], off_lo a4, off_hi a5) */
		return sys_pread(proc, (int)a0, (void *)(uintptr_t)a1, (size_t)a2, (uint32_t)a4);
	case OVE_LNX_NR_pwrite64: /* (fd, buf, count, [pad a3], off_lo a4, off_hi a5) */
		return sys_pwrite(proc, (int)a0, (const void *)(uintptr_t)a1, (size_t)a2,
				  (uint32_t)a4);
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
	case OVE_LNX_NR__llseek:
		return sys_llseek(proc, (int)a0, (unsigned long)a1, (unsigned long)a2,
				  (uint64_t *)(uintptr_t)a3, (unsigned int)a4);
	case OVE_LNX_NR_ftruncate64:
		/* 64-bit length is register-pair aligned on ARM: fd=a0, len=(a2,a3). */
		return sys_ftruncate(proc, (int)a0,
				     (uint64_t)(uint32_t)a2 | ((uint64_t)(uint32_t)a3 << 32));
	case OVE_LNX_NR_fstat64:
		return sys_fstat64(proc, (int)a0, (void *)(uintptr_t)a1);
	case OVE_LNX_NR_stat64: /* (path, statbuf) — follows symlinks */
		return sys_stat_path(proc, (const char *)(uintptr_t)a0, 1, (void *)(uintptr_t)a1);
	case OVE_LNX_NR_lstat64: /* (path, statbuf) — does NOT follow */
		return sys_stat_path(proc, (const char *)(uintptr_t)a0, 0, (void *)(uintptr_t)a1);
	case OVE_LNX_NR_fstatat64: /* (dirfd, path, statbuf, flags) */
		return sys_stat_path(proc, (const char *)(uintptr_t)a1,
				     !((int)a3 & OVE_LNX_AT_SYMLINK_NOFOLLOW),
				     (void *)(uintptr_t)a2);
	case OVE_LNX_NR_readlink: /* (path, buf, bufsiz) */
		return sys_readlink(proc, (const char *)(uintptr_t)a0, (char *)(uintptr_t)a1,
				    (size_t)a2);
	case OVE_LNX_NR_readlinkat: /* (dirfd, path, buf, bufsiz) */
		return sys_readlink(proc, (const char *)(uintptr_t)a1, (char *)(uintptr_t)a2,
				    (size_t)a3);
	case OVE_LNX_NR_access: /* (path, mode) — mode ignored */
		return sys_access(proc, (const char *)(uintptr_t)a0);
	case OVE_LNX_NR_faccessat:  /* (dirfd, path, mode) */
	case OVE_LNX_NR_faccessat2: /* (dirfd, path, mode, flags) */
		return sys_access(proc, (const char *)(uintptr_t)a1);
	case OVE_LNX_NR_mkdir: /* (path, mode) */
		return sys_mkdir(proc, (const char *)(uintptr_t)a0, (uint32_t)a1);
	case OVE_LNX_NR_mkdirat: /* (dirfd, path, mode) */
		return sys_mkdir(proc, (const char *)(uintptr_t)a1, (uint32_t)a2);
	case OVE_LNX_NR_rmdir: /* (path) */
		return sys_unlink(proc, (const char *)(uintptr_t)a0, 1);
	case OVE_LNX_NR_unlink: /* (path) */
		return sys_unlink(proc, (const char *)(uintptr_t)a0, 0);
	case OVE_LNX_NR_unlinkat: /* (dirfd, path, flags) */
		return sys_unlink(proc, (const char *)(uintptr_t)a1,
				  ((int)a2 & OVE_LNX_AT_REMOVEDIR) ? 1 : 0);
	case OVE_LNX_NR_rename: /* (oldpath, newpath) */
		return sys_rename(proc, (const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
	case OVE_LNX_NR_renameat:  /* (olddirfd, old, newdirfd, new) */
	case OVE_LNX_NR_renameat2: /* (olddirfd, old, newdirfd, new, flags) */
		return sys_rename(proc, (const char *)(uintptr_t)a1, (const char *)(uintptr_t)a3);
	case OVE_LNX_NR_symlink: /* (target, linkpath) */
		return sys_symlink(proc, (const char *)(uintptr_t)a0, (const char *)(uintptr_t)a1);
	case OVE_LNX_NR_symlinkat: /* (target, newdirfd, linkpath) */
		return sys_symlink(proc, (const char *)(uintptr_t)a0, (const char *)(uintptr_t)a2);
	case OVE_LNX_NR_chmod: /* (path, mode) */
		return sys_chmod(proc, (const char *)(uintptr_t)a0, (uint32_t)a1);
	case OVE_LNX_NR_fchmodat: /* (dirfd, path, mode) */
		return sys_chmod(proc, (const char *)(uintptr_t)a1, (uint32_t)a2);
	case OVE_LNX_NR_utimensat:	  /* (dirfd, path, times, flags) — times not tracked */
	case OVE_LNX_NR_utimensat_time64: /* time64 variant uClibc-ng issues for touch */
		return sys_utimensat(proc, (const char *)(uintptr_t)a1);
	case OVE_LNX_NR_mount:	 /* synthetic /proc + overlay are always present */
	case OVE_LNX_NR_umount2: /* (rcS does `mount -t proc proc /proc`) */
		return 0;
	case OVE_LNX_NR_statfs64:  /* (path, sz, buf) */
	case OVE_LNX_NR_fstatfs64: /* (fd, sz, buf) */
		return sys_statfs(proc, (void *)(uintptr_t)a2);
	case OVE_LNX_NR_getrandom: /* (buf, count, flags) */
		return sys_getrandom(proc, (void *)(uintptr_t)a0, (size_t)a1);
	case OVE_LNX_NR_sysinfo: { /* uptime + ram totals (uptime/free read this) */
		struct ove_lnx_sysinfo {
			int32_t uptime;
			uint32_t loads[3];
			uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
			uint16_t procs, pad;
			uint32_t totalhigh, freehigh, mem_unit;
			char _f[8];
		} *si = (void *)(uintptr_t)a0;
		if (!user_ok(proc, si, sizeof(*si), 1))
			return -OVE_LNX_EFAULT;
		memset(si, 0, sizeof(*si));
		uint64_t ns = 0;
		ove_time_get_ns(&ns);
		si->uptime = (int32_t)(ns / 1000000000ull);
		si->totalram = 4u * 1024u * 1024u;
		si->freeram = 2u * 1024u * 1024u;
		si->procs = 2;
		si->mem_unit = 1;
		return 0;
	}
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
#if defined(CONFIG_OVE_LINUX_DEV)
					if (s->kind == OVE_LNX_FD_DEV)
						ove_lnx_dev_get(s->file_idx);
#endif
#if defined(CONFIG_OVE_LINUX_NET)
					if (s->kind == OVE_LNX_FD_SOCKET)
						ove_lnx_sock_get(s->file_idx);
#endif
					return nfd;
				}
			}
			return -OVE_LNX_EMFILE;
		}
#if defined(CONFIG_OVE_LINUX_DEV)
		/* A device fd honours F_SETFL/F_GETFL so O_NONBLOCK takes effect (LVGL's
		 * evdev opens blocking, then fcntl(F_SETFL, O_NONBLOCK)). */
		if (s->kind == OVE_LNX_FD_DEV) {
			if ((int)a1 == OVE_LNX_F_SETFL) {
				ove_lnx_dev_setfl(s->file_idx, (int)a2);
				return 0;
			}
			if ((int)a1 == OVE_LNX_F_GETFL)
				return ove_lnx_dev_getfl(s->file_idx);
		}
#endif
#if defined(CONFIG_OVE_LINUX_NET)
		/* A socket fd honours F_SETFL/F_GETFL so O_NONBLOCK gates parking. */
		if (s->kind == OVE_LNX_FD_SOCKET) {
			if ((int)a1 == OVE_LNX_F_SETFL) {
				ove_lnx_sock_setfl(s->file_idx, (int)a2);
				return 0;
			}
			if ((int)a1 == OVE_LNX_F_GETFL)
				return ove_lnx_sock_getfl(s->file_idx);
		}
#endif
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
		/* getcwd(buf, size): write the cwd; the raw syscall returns the length
		 * including the NUL terminator. */
		char *buf = (char *)(uintptr_t)a0;
		if (!buf)
			return -OVE_LNX_EFAULT;
		size_t len = strlen(proc->cwd) + 1;
		if ((size_t)a1 < len)
			return -OVE_LNX_ERANGE;
		if (!user_ok(proc, buf, len, 1))
			return -OVE_LNX_EFAULT;
		memcpy(buf, proc->cwd, len);
		return (long)len;
	}
	case OVE_LNX_NR_chdir: {
		const char *path = (const char *)(uintptr_t)a0;
		if (!path)
			return -OVE_LNX_EFAULT;
		char abspath[OVE_LNX_PATH_MAX];
		long r = resolve_path(proc, path, abspath, sizeof(abspath));
		if (r < 0)
			return r;
		/* "/" is always valid; else require an existing directory in either the
		 * writable overlay or the read-only rootfs. */
		if (!(abspath[0] == '/' && abspath[1] == '\0')) {
			int wi = wfs_find(abspath);
			if (wi >= 0) {
				if ((g_wnodes[wi].mode & OVE_LNX_S_IFMT) != OVE_LNX_S_IFDIR)
					return -OVE_LNX_ENOTDIR;
			} else {
				int idx = fs_lookup(proc, abspath);
				if (idx < 0)
					return -OVE_LNX_ENOENT;
				if ((file_mode(&proc->fs[idx]) & OVE_LNX_S_IFMT) != OVE_LNX_S_IFDIR)
					return -OVE_LNX_ENOTDIR;
			}
		}
		strcpy(proc->cwd, abspath);
		return 0;
	}
	case OVE_LNX_NR_umask: { /* set the file-creation mask, return the previous (per-proc, inherited) */
		int old = proc->umask;
		proc->umask = (unsigned short)(a0 & 0777);
		return old;
	}
	case OVE_LNX_NR_prctl:
	case OVE_LNX_NR_setpgid:
	case OVE_LNX_NR_sync:	/* no backing store to flush */
	case OVE_LNX_NR_fchmod: /* modes/ownership not tracked (login chmods the tty) */
	case OVE_LNX_NR_fchown32:
	case OVE_LNX_NR_setgroups32: /* uid/gid not enforced (login's privilege drop is */
	case OVE_LNX_NR_setuid32:    /* inert — programs run privileged in this tier) */
	case OVE_LNX_NR_setgid32:
		return 0; /* process-control / fs-mode setup accepted (inert) */
	case OVE_LNX_NR_setitimer: { /* (which, new, old) — ITIMER_REAL -> SIGALRM (alarm()) */
		int which = (int)a0;
		const void *unew = (const void *)(uintptr_t)a1;
		void *uold = (void *)(uintptr_t)a2;
		if (which != OVE_LNX_ITIMER_REAL)
			return 0; /* only the real-time timer (login timeout, ping interval) */
		/* struct itimerval { timeval it_interval; timeval it_value; }; ARM32 long=4,
		 * so it is 4 x u32: [interval_sec, interval_usec, value_sec, value_usec]. */
		uint64_t now = 0;
		ove_time_get_us(&now);
		if (uold) {
			if (!user_ok(proc, uold, 16, 1))
				return -OVE_LNX_EFAULT;
			uint32_t ov[4] = {0, 0, 0, 0};
			uint64_t rem = (proc->alarm_deadline_us && proc->alarm_deadline_us > now)
					       ? proc->alarm_deadline_us - now
					       : 0;
			ov[0] = (uint32_t)(proc->alarm_interval_us / 1000000u);
			ov[1] = (uint32_t)(proc->alarm_interval_us % 1000000u);
			ov[2] = (uint32_t)(rem / 1000000u);
			ov[3] = (uint32_t)(rem % 1000000u);
			memcpy(uold, ov, 16);
		}
		if (!unew)
			return 0;
		if (!user_ok(proc, unew, 16, 0))
			return -OVE_LNX_EFAULT;
		uint32_t nv[4];
		memcpy(nv, unew, 16);
		proc->alarm_interval_us = (uint64_t)nv[0] * 1000000u + nv[1];
		uint64_t val_us = (uint64_t)nv[2] * 1000000u + nv[3];
		proc->alarm_deadline_us = val_us ? now + val_us : 0; /* it_value 0 disarms */
		return 0;
	}
	case OVE_LNX_NR_getpgrp:   /* shell job control: process group == pid */
	case OVE_LNX_NR_setsid:	   /* getty/login start a new session */
		return proc->pid;  /* the caller becomes the session/group leader */
	case OVE_LNX_NR_reboot: {  /* reboot(magic1, magic2, cmd, arg) — cmd is a2 */
		unsigned cmd = (unsigned)a2;
		/* Only an actual halt/poweroff/restart stops the system; init calls
		 * reboot(CAD_OFF=0) at startup to disable Ctrl-Alt-Del — a no-op here. */
		if (cmd == 0x01234567u /* RESTART */ || cmd == 0xcdef0123u /* HALT */ ||
		    cmd == 0x4321fedcu /* POWER_OFF */ || cmd == 0xa1b2c3d4u /* RESTART2 */) {
			g_ove_lnx_halt = 1;
			proc->exited = 1;
			proc->exit_status = 0;
		}
		return 0;
	}
	case OVE_LNX_NR_gettid:
		return proc->pid;	 /* single-threaded: tid == pid */
	case OVE_LNX_NR_clock_gettime: { /* (clockid, struct timespec*) — 32-bit time_t */
		int32_t *ts = (int32_t *)(uintptr_t)a1;
		if (!user_ok(proc, ts, 2 * sizeof(int32_t), 1))
			return -OVE_LNX_EFAULT;
		uint64_t sec;
		uint32_t nsec;
		now_sec_nsec((int)a0, &sec, &nsec);
		ts[0] = (int32_t)sec;
		ts[1] = (int32_t)nsec;
		return 0;
	}
	case OVE_LNX_NR_clock_gettime64: { /* (clockid, struct __kernel_timespec*) — 64-bit */
		int64_t *ts = (int64_t *)(uintptr_t)a1;
		if (!user_ok(proc, ts, 2 * sizeof(int64_t), 1))
			return -OVE_LNX_EFAULT;
		uint64_t sec;
		uint32_t nsec;
		now_sec_nsec((int)a0, &sec, &nsec);
		ts[0] = (int64_t)sec;
		ts[1] = (int64_t)nsec;
		return 0;
	}
	case OVE_LNX_NR_gettimeofday: { /* (struct timeval*, tz) */
		int32_t *tv = (int32_t *)(uintptr_t)a0;
		if (!user_ok(proc, tv, 2 * sizeof(int32_t), 1))
			return -OVE_LNX_EFAULT;
		uint64_t sec;
		uint32_t nsec;
		now_sec_nsec(0, &sec, &nsec);
		tv[0] = (int32_t)sec;
		tv[1] = (int32_t)(nsec / 1000u);
		return 0;
	}
	case OVE_LNX_NR_nanosleep:	 /* (req, rem) */
	case OVE_LNX_NR_clock_nanosleep: /* (clockid, flags, req, rem) */
	case OVE_LNX_NR_clock_nanosleep_time64: {
		/* Record a wake deadline and ask the run loop to park + delay this proc
		 * (the trap context cannot block). The run loop aborts the slot for the
		 * duration so the RTOS idle/kernel/other threads run and real time + CPU
		 * stats advance — which is what top needs between its two samples. */
		uintptr_t reqp = (nr == OVE_LNX_NR_nanosleep) ? (uintptr_t)a0 : (uintptr_t)a2;
		if (!user_ok(proc, (const void *)reqp,
			     (nr == OVE_LNX_NR_clock_nanosleep_time64) ? 16u : 8u, 0))
			return -OVE_LNX_EFAULT;
		uint64_t sec, nsec;
		if (nr == OVE_LNX_NR_clock_nanosleep_time64) {
			const int64_t *t = (const int64_t *)reqp; /* time64 {sec, nsec} */
			sec = (uint64_t)t[0];
			nsec = (uint64_t)t[1];
		} else {
			const int32_t *t = (const int32_t *)reqp; /* time32 {sec, nsec} */
			sec = (uint64_t)(uint32_t)t[0];
			nsec = (uint64_t)(uint32_t)t[1];
		}
		uint64_t dur_us = sec * 1000000ull + nsec / 1000ull;
		if (dur_us > 100000000ull)
			dur_us = 100000000ull; /* clamp to 100 s */
		/* Use the FreeRTOS TICK (ove_time_get_us), NOT the DWT (ove_time_get_ns): with
		 * configUSE_TICKLESS_IDLE the idle task WFI-sleeps between events, gating the CPU clock
		 * so the DWT cycle counter FREEZES across the sleep, while the tick is re-accounted by
		 * vTaskStepTick() on wake. The run-loop coordinator compares this deadline against
		 * ove_time_get_us, so both must use the same cross-idle clock or every sleep / poll
		 * timeout drifts (on real silicon interactive top ran ~1.66x slow + un-quittable). */
		uint64_t now_us = 0;
		ove_time_get_us(&now_us);
		proc->sleep_until_us = now_us + dur_us;
		proc->sleep_pending = 1;
		return 0;
	}
	case OVE_LNX_NR_uname: {
		/* struct utsname: 6 fixed 65-byte fields (sysname, nodename, release,
		 * version, machine, domainname). The shell reads these at startup. */
		char *u = (char *)(uintptr_t)a0;
		if (!user_ok(proc, u, 6 * 65, 1))
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
		if (act && !user_ok(proc, act, 3 * sizeof(uint32_t), 0))
			return -OVE_LNX_EFAULT;
		if (oact && !user_ok(proc, oact, 3 * sizeof(uint32_t), 1))
			return -OVE_LNX_EFAULT;
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
		ove_lnx_pollfd *pfds = (ove_lnx_pollfd *)(uintptr_t)a0;
		unsigned nfds = (unsigned)a1;
		if (nfds && !user_ok(proc, pfds, (size_t)nfds * sizeof(ove_lnx_pollfd), 1))
			return -OVE_LNX_EFAULT;
		/* Timeout: poll(2) passes ms in a2 (<0 = block); ppoll passes a struct
		 * timespec* (NULL = block). A SHORT finite timeout means the caller is
		 * probing for input that might *immediately* follow — e.g. vi/hush's
		 * read_key polling ~50 ms after ESC to tell a lone ESC from an escape
		 * sequence. We keep no read-ahead, so honestly report "no data yet" for
		 * such probes: a lone ESC then stays ESC (Esc then :q works in vi). vi
		 * also uses poll(timeout 0) as "is input pending? if not, repaint the
		 * screen" — reporting ready there made it never repaint while inserting
		 * (edits stayed invisible). A blocking/long poll reports ready and the
		 * caller blocks in read() for the real byte (the console read blocks
		 * until a key arrives). */
		long tmo_ms;
		if (nr == OVE_LNX_NR_poll) {
			tmo_ms = (long)(int32_t)a2;
		} else {
			const int64_t *ts = (const int64_t *)(uintptr_t)a2; /* {sec, nsec} */
			if (ts && !user_ok(proc, ts, 2 * sizeof(int64_t), 0))
				return -OVE_LNX_EFAULT;
			tmo_ms = ts ? (long)(ts[0] * 1000 + ts[1] / 1000000) : -1;
		}
		/* With a console_poll callback (UART console) we report the console fd's REAL
		 * readiness, enabling interactive top's `q` quit; without one a short finite
		 * timeout is a read_key probe (vi/hush ESC + "input pending?") reported
		 * not-ready (no read-ahead), and a longer/blocking poll reports ready so the
		 * caller blocks in read() for the byte. */
		int probe = (tmo_ms >= 0 && tmo_ms <= 100);
		int key = (proc->console_poll && proc->console_poll(proc->io_ctx) > 0);
		int ready = 0;
#if defined(CONFIG_OVE_LINUX_NET)
		int has_socket = 0;
#endif
		for (unsigned i = 0; i < nfds; i++) {
			pfds[i].revents = 0;
			ove_lnx_fd_t *s = fd_slot(proc, pfds[i].fd);
			if (!s)
				continue;
			int avail;
			if (s->kind == OVE_LNX_FD_CONSOLE)
				avail = (tmo_ms < 0) ? 1 : (proc->console_poll ? key : !probe);
#if defined(CONFIG_OVE_LINUX_DEV)
			else if (s->kind == OVE_LNX_FD_DEV) {
				/* Report the driver's real readiness bits (fb POLLOUT, evdev
				 * POLLIN when the event ring is non-empty). */
				unsigned pb = ove_lnx_dev_poll(s->file_idx);
				pfds[i].revents = (short)(pfds[i].events & pb &
							  (OVE_LNX_POLLIN | OVE_LNX_POLLOUT));
				if (pfds[i].revents)
					ready++;
				continue;
			}
#endif
#if defined(CONFIG_OVE_LINUX_NET)
			else if (s->kind == OVE_LNX_FD_SOCKET) {
				unsigned pb = ove_lnx_sock_poll(s->file_idx);
				pfds[i].revents = (short)(pfds[i].events & pb &
							  (OVE_LNX_POLLIN | OVE_LNX_POLLOUT));
				if (pfds[i].revents)
					ready++;
				has_socket = 1;
				continue;
			}
#endif
			else
				avail = 1; /* files / pipes: readable/writable */
			if (avail) {
				pfds[i].revents = pfds[i].events &
						  (OVE_LNX_POLLIN | OVE_LNX_POLLOUT);
				if (pfds[i].revents)
					ready++;
			}
		}
		if (ready > 0 || tmo_ms == 0)
			return ready;
#if defined(CONFIG_OVE_LINUX_NET)
		/* A blocking poll whose set includes a socket parks on SOCKW_POLL: the
		 * coordinator re-scans readiness on its <=5 ms socket-retry tick (via
		 * ove_lnx_poll_retry) and resumes us when an fd becomes ready or the timeout
		 * elapses. Without this a socket poll would sleep the whole timeout and return
		 * 0, breaking the uClibc DNS resolver (poll(POLLIN) then recv(MSG_DONTWAIT)). */
		if (has_socket) {
			proc->sock_buf = (uintptr_t)pfds;
			proc->sock_len = nfds;
			if (tmo_ms > 0) {
				uint64_t now_us = 0;
				ove_time_get_us(&now_us);
				proc->sock_deadline_us = now_us + (uint64_t)tmo_ms * 1000ull;
			} else {
				proc->sock_deadline_us = UINT64_MAX; /* poll(-1): block forever */
			}
			proc->sock_oi = -1; /* the retry re-scans the whole set, not one open */
			proc->sock_wait = OVE_LNX_SOCKW_POLL;
			return 0; /* parked; coordinator resumes with the ready count / 0 */
		}
#endif
		/* Nothing ready + a real timeout: with the UART console, park for the timeout
		 * (paces interactive top's refresh, returns 0); a buffered keystroke is caught
		 * at the next poll. Without console_poll a long timeout already reported ready
		 * above, so we only reach here on a no-callback probe → return 0. */
		if (proc->console_poll && tmo_ms > 0) {
			/* TICK (cross-idle), not DWT: tickless idle freezes the DWT while the proc is
			 * parked here, and the coordinator checks this against ove_time_get_us (see the
			 * nanosleep handler). Both must use the same clock or top's refresh + q drift. */
			uint64_t now_us = 0;
			ove_time_get_us(&now_us);
			proc->sleep_until_us = now_us + (uint64_t)tmo_ms * 1000ull;
			proc->sleep_pending = 1;
		}
		return 0;
	}
	case OVE_LNX_NR_wait4: {
		if (a1 && !user_ok(proc, (void *)(uintptr_t)a1, sizeof(int), 1))
			return -OVE_LNX_EFAULT; /* the kernel WRITES *status */
		/* Reap an already-exited child immediately (FIFO; status = exit_code << 8).
		 * Else, if children are still live, BLOCK: set wait_pending so the dispatch
		 * parks us and the run-loop coordinator resumes us (returning the reaped pid
		 * + writing *status) when one exits. No children at all → -ECHILD. */
		if (proc->child_count > 0) {
			int pid = proc->child_pid[0];
			int code = proc->child_status[0];
			for (int i = 1; i < proc->child_count; i++) {
				proc->child_pid[i - 1] = proc->child_pid[i];
				proc->child_status[i - 1] = proc->child_status[i];
			}
			proc->child_count--;
			int *status = (int *)(uintptr_t)a1;
			if (status)
				*status = (code > 128 && code <= 128 + 31) ? (code - 128)
									   : ((code & 0xff) << 8);
			return pid;
		}
		if (proc->live_children == 0)
			return -OVE_LNX_ECHILD;
		if ((int)a2 & 1) /* WNOHANG: children live but none ready */
			return 0;
		proc->wait_pending = 1;
		proc->wait_pid = (int)a0;
		proc->wait_status_p = (uintptr_t)a1;
		return 0; /* dispatch parks; the coordinator's resume supplies the real r0 */
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
		if (!tty)
			return -OVE_LNX_ENOTTY;
#if defined(CONFIG_OVE_LINUX_DEV)
		/* Device ioctls (FBIOGET_VSCREENINFO, EVIOCG*, ...) dispatch to the driver
		 * BEFORE the console-only tty gate below (which would else -ENOTTY them). */
		if (tty->kind == OVE_LNX_FD_DEV)
			return ove_lnx_dev_ioctl(proc, tty->file_idx, (unsigned long)a1,
						 (unsigned long)a2);
#endif
#if defined(CONFIG_OVE_LINUX_NET)
		/* Socket ioctls: SIOC* interface config (ifconfig/route) — before the tty gate. */
		if (tty->kind == OVE_LNX_FD_SOCKET)
			return ove_lnx_sock_ioctl(proc, (unsigned long)a1, (unsigned long)a2);
#endif
		if (tty->kind != OVE_LNX_FD_CONSOLE)
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
		case OVE_LNX_TIOCSCTTY: /* getty/login: become/drop/set the tty session */
		case OVE_LNX_TIOCNOTTY:
		case OVE_LNX_TIOCSPGRP:
			return 0;
		case OVE_LNX_TIOCGPGRP: {
			int *pgrp = (int *)(uintptr_t)a2;
			if (pgrp)
				*pgrp = proc->pid;
			return 0;
		}
		default:
			return -OVE_LNX_ENOTTY;
		}
	}
	case OVE_LNX_NR_rt_sigsuspend: {
		/* LinuxThreads suspend(): block until a signal (the restart) is delivered. If one is
		 * already pending (a restart that beat us here), fall through so the dispatch delivers
		 * it now; otherwise ask the run loop to park us — the coordinator runs the handler on
		 * the restart kill() and resumes us. sigsuspend always "returns" -EINTR. The mask arg
		 * is honoured loosely: any delivered signal wakes us, matching the restart protocol
		 * (the restart signal is the only one sent to a suspended thread). */
		if (!proc->pending_sig)
			proc->sigsuspend_pending = 1;
		return -OVE_LNX_EINTR;
	}
	case OVE_LNX_NR_rt_sigprocmask:
		return 0;
	case OVE_LNX_NR_set_tid_address:
		return 1; /* our single thread's tid */
	case OVE_LNX_NR_set_robust_list:
		return 0;
	case OVE_LNX_NR_futex:
	case OVE_LNX_NR_futex_time64: {
		/* uClibc-ng DOES do NOMMU pthreads (LinuxThreads; this build has
		 * UCLIBC_HAS_THREADS=y), but this personality handles every clone() — incl. a
		 * thread's clone(CLONE_VM) — as a VFORK: the parent is suspended until the child
		 * execs (into its own region) or exits (ove_lnx_run.c). A pthread thread never
		 * execs, so it can't co-run with its parent, and there is no uaddr-keyed futex
		 * queue — i.e. no concurrent threads for a futex to coordinate. BusyBox is
		 * single-threaded anyway, so the futexes we see are libc-internal lock
		 * fallbacks: reply benignly (WAIT-family → -EAGAIN, the caller retries its
		 * uncontended userspace lock; WAKE etc. → 0) instead of a noisy -ENOSYS. Real
		 * threads — a co-running clone(CLONE_VM) in the shared region + a true futex
		 * wait/wake — are a future item. */
		int op = (int)a1 & 0x7f; /* mask FUTEX_PRIVATE_FLAG / FUTEX_CLOCK_REALTIME */
		return (op == 0 || op == 9) ? -OVE_LNX_EAGAIN : 0; /* WAIT / WAIT_BITSET */
	}
#if defined(CONFIG_OVE_LINUX_NET)
	case OVE_LNX_NR_socket: { /* (domain, type, protocol) */
		long oi = ove_lnx_sock_new((int)a0, (int)a1, (int)a2);
		if (oi < 0)
			return oi;
		int fd = fd_alloc(proc, OVE_LNX_FD_SOCKET, (int)oi, 0);
		if (fd < 0) {
			ove_lnx_sock_close((int)oi);
			return -OVE_LNX_EMFILE;
		}
		return fd;
	}
	case OVE_LNX_NR_connect: { /* (fd, addr, addrlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_connect(proc, s->file_idx, (const void *)(uintptr_t)a1,
					    (unsigned)a2);
	}
	case OVE_LNX_NR_send:	  /* (fd, buf, len, flags) */
	case OVE_LNX_NR_sendto: { /* (fd, buf, len, flags, dest, destlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		const void *dest = (nr == OVE_LNX_NR_sendto) ? (const void *)(uintptr_t)a4 : NULL;
		return ove_lnx_sock_send(proc, s->file_idx, (const void *)(uintptr_t)a1, (size_t)a2,
					 (int)a3, dest, (unsigned)a5);
	}
	case OVE_LNX_NR_recv:	    /* (fd, buf, len, flags) */
	case OVE_LNX_NR_recvfrom: { /* (fd, buf, len, flags, src, srclen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		void *src = (nr == OVE_LNX_NR_recvfrom) ? (void *)(uintptr_t)a4 : NULL;
		void *srclen = (nr == OVE_LNX_NR_recvfrom) ? (void *)(uintptr_t)a5 : NULL;
		return ove_lnx_sock_recv(proc, s->file_idx, (void *)(uintptr_t)a1, (size_t)a2, (int)a3,
					 src, srclen);
	}
	case OVE_LNX_NR_shutdown: { /* (fd, how) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_shutdown(s->file_idx, (int)a1);
	}
	case OVE_LNX_NR_getsockname: { /* (fd, addr, addrlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_getsockname(proc, s->file_idx, (void *)(uintptr_t)a1,
						(void *)(uintptr_t)a2);
	}
	case OVE_LNX_NR_getpeername: { /* (fd, addr, addrlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_getpeername(proc, s->file_idx, (void *)(uintptr_t)a1,
						(void *)(uintptr_t)a2);
	}
	case OVE_LNX_NR_setsockopt: { /* (fd, level, optname, optval, optlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_setsockopt(proc, s->file_idx, (int)a1, (int)a2,
					       (const void *)(uintptr_t)a3, (unsigned)a4);
	}
	case OVE_LNX_NR_getsockopt: { /* (fd, level, optname, optval, optlen) */
		ove_lnx_fd_t *s = fd_slot(proc, (int)a0);
		if (!s || s->kind != OVE_LNX_FD_SOCKET)
			return -OVE_LNX_ENOTSOCK;
		return ove_lnx_sock_getsockopt(proc, s->file_idx, (int)a1, (int)a2,
					       (void *)(uintptr_t)a3, (void *)(uintptr_t)a4);
	}
	case OVE_LNX_NR_bind:	    /* server sockets (bind/listen/accept) land in P4 */
	case OVE_LNX_NR_listen:
	case OVE_LNX_NR_accept:
	case OVE_LNX_NR_accept4:
	case OVE_LNX_NR_socketpair:
	case OVE_LNX_NR_sendmsg: /* scatter/gather lands in P1 */
	case OVE_LNX_NR_recvmsg:
		return -OVE_LNX_EOPNOTSUPP;
#endif
	default:
		return -OVE_LNX_ENOSYS;
	}
}

#if defined(CONFIG_OVE_LINUX_NET)
/* Re-evaluate a parked poll(2)'s fd set for readiness (socket + device + console).
 * Mirrors the initial sys_poll scan but in blocking mode — a console fd reports its
 * real key readiness rather than the vi/top ESC-probe heuristic. */
static int ove_lnx_poll_scan(ove_lnx_proc_t *proc, ove_lnx_pollfd *pfds, unsigned nfds)
{
	int key = (proc->console_poll && proc->console_poll(proc->io_ctx) > 0);
	int ready = 0;
	for (unsigned i = 0; i < nfds; i++) {
		pfds[i].revents = 0;
		ove_lnx_fd_t *s = fd_slot(proc, pfds[i].fd);
		if (!s)
			continue;
		unsigned pb;
		if (s->kind == OVE_LNX_FD_SOCKET)
			pb = ove_lnx_sock_poll(s->file_idx);
#if defined(CONFIG_OVE_LINUX_DEV)
		else if (s->kind == OVE_LNX_FD_DEV)
			pb = ove_lnx_dev_poll(s->file_idx);
#endif
		else if (s->kind == OVE_LNX_FD_CONSOLE)
			pb = (unsigned)((proc->console_poll ? (key ? OVE_LNX_POLLIN : 0)
							    : OVE_LNX_POLLIN) |
					OVE_LNX_POLLOUT);
		else
			pb = OVE_LNX_POLLIN | OVE_LNX_POLLOUT; /* files/pipes always ready */
		pfds[i].revents =
			(short)(pfds[i].events & pb & (OVE_LNX_POLLIN | OVE_LNX_POLLOUT));
		if (pfds[i].revents)
			ready++;
	}
	return ready;
}

long ove_lnx_poll_retry(ove_lnx_proc_t *proc)
{
	ove_lnx_pollfd *pfds = (ove_lnx_pollfd *)(uintptr_t)proc->sock_buf;
	int ready = ove_lnx_poll_scan(proc, pfds, (unsigned)proc->sock_len);
	if (ready > 0)
		return ready;
	if (proc->sock_deadline_us != UINT64_MAX) {
		uint64_t now_us = 0;
		ove_time_get_us(&now_us);
		if (now_us >= proc->sock_deadline_us)
			return 0; /* timed out */
	}
	return -OVE_LNX_EAGAIN; /* still waiting */
}
#endif /* CONFIG_OVE_LINUX_NET */

#endif /* CONFIG_OVE_LINUX */
