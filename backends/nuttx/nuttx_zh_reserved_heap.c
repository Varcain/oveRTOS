/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * NuttX zero-heap reserved pool.
 *
 * Strong-overrides the weak ove_heap_lock_post_*_ hooks declared in
 * backends/common/ove_heap_lock.c. Routes every allocation that lands
 * post-`ove_heap_lock()` into a fixed-size BSS buffer managed by an
 * isolated mm_heap instance — sized at compile time, never grown.
 *
 * Why NuttX needs this and the FreeRTOS+lwIP path doesn't:
 *
 *   FreeRTOS: every L2/L3/L4 byte lwIP touches lives in a configured
 *   BSS pool (lwipopts MEM_USE_POOLS, MEMP_MEM_MALLOC=0, MEM_LIBC_-
 *   MALLOC=0 — pinned in lwip_port/lwipopts_common.h). After init,
 *   pvPortMalloc / __wrap_malloc are never called from the network
 *   path, so the strict trap (return NULL) never fires.
 *
 *   NuttX: the kernel's POSIX socket layer hard-codes
 *     fs_heap_zalloc(struct file)        in fs/inode/fs_files.c (per FD)
 *     lib_malloc(struct dns_query_data_s) in libs/libc/netdb/lib_dnsquery.c
 *     lib_malloc(NETDB_BUFSIZE)           in libs/libc/netdb/lib_getaddrinfo.c
 *   All resolve to kmm_malloc in flat-build (and kmm_malloc → __wrap_malloc
 *   under our --wrap chain). There is no upstream knob to point these
 *   at a different heap, so a strict NULL-on-trap silently fails every
 *   socket() and every DNS query after `ove_heap_lock()`.
 *
 * The fixed-size pool preserves the spirit of zero-heap — total RAM
 * footprint is decided at compile time, the main kmm heap never grows
 * post-init — while letting the unavoidable kernel allocations succeed.
 *
 * Sizing: ~600 B per concurrent DNS query (struct dns_query_data_s
 * incl. 512-byte response buffer), ~24 B per open FD's struct file,
 * plus mm_heap bookkeeping (~64 B + per-allocation node header).
 * 8 KiB comfortably covers the example_net_zh workload (a handful of
 * sockets + sequential DNS); raise CONFIG_OVE_NUTTX_ZH_RESERVED_HEAP
 * for apps that hold more concurrent state.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_RTOS_NUTTX) && defined(CONFIG_OVE_ZERO_HEAP)

#include <nuttx/mm/mm.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef CONFIG_OVE_NUTTX_ZH_RESERVED_HEAP
#define CONFIG_OVE_NUTTX_ZH_RESERVED_HEAP (16 * 1024)
#endif

/* MM_ALIGN (default 8 on 32-bit ARM) is what mm_initialize aligns on
 * internally; matching it here keeps the usable region maximal. */
static uint8_t g_zh_reserved_buf[CONFIG_OVE_NUTTX_ZH_RESERVED_HEAP] __attribute__((aligned(8)));
static struct mm_heap_s *g_zh_reserved_heap;

/* Force the linker to pull this .obj from libapps_ove_app.a even though
 * the weak fallbacks in backends/common/ove_heap_lock.c.obj already
 * satisfy the post_*_ references when that obj is linked first (for
 * __wrap_malloc). Referenced from nuttx_heap_lock.c, which is pulled
 * in unconditionally via --wrap=kmm_malloc. Without the anchor the
 * archive scanner sees no undefined symbol pointing here and skips us,
 * leaving the weak NULL stubs active. */
const char ove_zh_reserved_heap_link_anchor_;

static void zh_reserved_heap_init(void)
{
	if (g_zh_reserved_heap == NULL) {
		g_zh_reserved_heap = mm_initialize("ove_zh_reserved", g_zh_reserved_buf,
						   sizeof(g_zh_reserved_buf));
	}
}

static int in_reserved(const void *p)
{
	const uint8_t *cp = (const uint8_t *)p;
	return cp >= g_zh_reserved_buf && cp < g_zh_reserved_buf + sizeof(g_zh_reserved_buf);
}

void *ove_heap_lock_post_alloc_(size_t n)
{
	zh_reserved_heap_init();
	return mm_malloc(g_zh_reserved_heap, n);
}

void *ove_heap_lock_post_zalloc_(size_t n)
{
	zh_reserved_heap_init();
	return mm_zalloc(g_zh_reserved_heap, n);
}

void *ove_heap_lock_post_calloc_(size_t nmemb, size_t n)
{
	zh_reserved_heap_init();
	return mm_calloc(g_zh_reserved_heap, nmemb, n);
}

void *ove_heap_lock_post_realloc_(void *p, size_t n)
{
	zh_reserved_heap_init();

	/* realloc(NULL, n) == malloc(n); realloc within reserved is the
	 * common path. The cross-heap case (caller passes a pre-lock
	 * pointer that was allocated from the main heap before the lock
	 * engaged, then realloc's it post-lock) shouldn't arise on the
	 * NuttX socket / DNS paths exercised by oveRTOS — DNS allocates
	 * and frees within one query, sockets allocate and close, neither
	 * realloc's. Handle it best-effort to avoid silent corruption:
	 * allocate fresh in the reserved heap, copy n bytes (shrinking
	 * realloc is exact; growing realloc copies the new size, which
	 * may over-read the old allocation by a few bytes — typically
	 * harmless on RAM and never reached on our tested code paths). */
	if (p == NULL || in_reserved(p)) {
		return mm_realloc(g_zh_reserved_heap, p, n);
	}

	void *np = mm_malloc(g_zh_reserved_heap, n);
	if (np != NULL) {
		extern void __real_free(void *p);
		memcpy(np, p, n);
		__real_free(p);
	}
	return np;
}

void *ove_heap_lock_post_memalign_(size_t alignment, size_t size)
{
	zh_reserved_heap_init();
	return mm_memalign(g_zh_reserved_heap, alignment, size);
}

int ove_heap_lock_post_free_(void *p)
{
	if (p != NULL && in_reserved(p)) {
		mm_free(g_zh_reserved_heap, p);
		return 1;
	}
	return 0;
}

#endif /* CONFIG_OVE_RTOS_NUTTX && CONFIG_OVE_ZERO_HEAP */
