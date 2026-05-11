/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Portable heap-lock + libc-malloc wrap.  Compiled into every RTOS
 * backend's link; each backend's CMake helper additionally wires
 * `-Wl,--wrap=malloc` (and friends) into the final exe link so calls
 * to libc malloc/free/calloc/realloc/zalloc/memalign route through
 * the wrappers below.
 *
 * The wrappers gate on the lock flag:
 *   - locked + test mode → return NULL + bump counter (test only)
 *   - locked, not test   → DEBUGASSERT-equivalent abort
 *   - unlocked           → forward to the real allocator
 *
 * Forwarding to the real allocator goes through `__real_<sym>` —
 * a name the GNU linker rewrites to the original (renamed) symbol
 * when --wrap=<sym> is in effect.  Per-backend files supply weak
 * fallbacks for the case when --wrap is NOT wired into the final
 * link (so the wrappers stay callable for direct test invocation).
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_ZERO_HEAP

#include "ove/app.h"

#include <stdatomic.h>
#include <stddef.h>
#include <assert.h>

#ifdef CONFIG_OVE_RTOS_NUTTX
#include <nuttx/sched.h>
#define OVE_HEAP_LOCK_TRAP_ABORT() DEBUGASSERT(0)
#else
/* Generic abort for FreeRTOS / Zephyr / POSIX.  Drops into the
    * platform's assert / abort path. */
#define OVE_HEAP_LOCK_TRAP_ABORT()   \
	do {                         \
		volatile int _x = 0; \
		(void)*&_x;          \
	} while (0)
#endif

/*
 * Atomics:
 *   g_ove_mm_locked   — 0 = free, 1 = locked.  Set by ove_heap_lock().
 *   g_ove_mm_test_mode — when set, the trap returns NULL and bumps the
 *                       counter instead of asserting.  Used by tests
 *                       so a deliberate post-lock alloc can be observed
 *                       without aborting the whole binary.
 *   g_ove_mm_trap_count — incremented on each trapped call in test mode.
 */
static atomic_int g_ove_mm_locked;
static atomic_int g_ove_mm_test_mode;
static atomic_int g_ove_mm_trap_count;

void ove_heap_lock(void)
{
	atomic_store(&g_ove_mm_locked, 1);
}

/* Test-only hooks — declared here, not in ove/app.h, so applications
 * don't accidentally depend on them.  Tests pull them in via plain
 * extern declarations. */
void ove_heap_lock_test_begin(void);
int ove_heap_lock_test_end(void);

void ove_heap_lock_test_begin(void)
{
	atomic_store(&g_ove_mm_trap_count, 0);
	atomic_store(&g_ove_mm_test_mode, 1);
	atomic_store(&g_ove_mm_locked, 1);
}

int ove_heap_lock_test_end(void)
{
	int n = atomic_load(&g_ove_mm_trap_count);
	atomic_store(&g_ove_mm_test_mode, 0);
	atomic_store(&g_ove_mm_locked, 0);
	return n;
}

/* Internal trap helper — exposed for use by RTOS-specific wrap files
 * (kmm_malloc on NuttX, k_malloc on Zephyr, pvPortMalloc on FreeRTOS). */
int ove_heap_lock_trapped_(void);

int ove_heap_lock_trapped_(void)
{
	if (atomic_load(&g_ove_mm_locked)) {
		if (atomic_load(&g_ove_mm_test_mode)) {
			atomic_fetch_add(&g_ove_mm_trap_count, 1);
			return 1; /* signal "denied, return NULL" */
		}
		OVE_HEAP_LOCK_TRAP_ABORT();
		return 1; /* unreachable in practice; caller returns NULL */
	}
	return 0;
}

/* ── Post-lock fallback hooks ────────────────────────────────────────
 *
 * Default behaviour when the lock fires: return NULL (FreeRTOS+lwIP
 * uses BSS-only pools, so no post-lock allocator should ever be hit).
 * RTOSes whose kernel POSIX paths hard-code malloc on the unavoidable
 * code paths — NuttX's fs_heap_zalloc(struct file) in fdlist_allocate,
 * lib_malloc in lib_dnsquery / lib_getaddrinfo — strong-override these
 * to redirect into a pre-allocated private mm_heap. The free hook
 * returns 1 if it claimed the pointer (releasing it back to the
 * private pool); 0 means the wrapper should fall through to
 * __real_free. */
__attribute__((weak)) void *ove_heap_lock_post_alloc_(size_t n)
{
	(void)n;
	return NULL;
}
__attribute__((weak)) void *ove_heap_lock_post_zalloc_(size_t n)
{
	(void)n;
	return NULL;
}
__attribute__((weak)) void *ove_heap_lock_post_calloc_(size_t nmemb, size_t n)
{
	(void)nmemb;
	(void)n;
	return NULL;
}
__attribute__((weak)) void *ove_heap_lock_post_realloc_(void *p, size_t n)
{
	(void)p;
	(void)n;
	return NULL;
}
__attribute__((weak)) void *ove_heap_lock_post_memalign_(size_t alignment, size_t size)
{
	(void)alignment;
	(void)size;
	return NULL;
}
__attribute__((weak)) int ove_heap_lock_post_free_(void *p)
{
	(void)p;
	return 0;
}

#ifdef CONFIG_OVE_RTOS_NUTTX
/* Pull nuttx_zh_reserved_heap.c.obj out of libapps_ove_app.a so its
 * strong overrides for ove_heap_lock_post_*_ beat the weak NULL stubs
 * above. The archive scanner only pulls .obj's that satisfy an
 * undefined external; once it linked this file in for __wrap_malloc
 * the post_*_ symbols look "satisfied" by the weak defs and the
 * reserved-heap obj never gets pulled. This anchor reference forces
 * it. (NuttX flat-build doesn't reference any kmm_* symbol — kmm_-
 * malloc is a macro for malloc — so --wrap=kmm_malloc does not pull
 * nuttx_heap_lock.c.obj either; the anchor has to live here, in the
 * file that IS already linked.) */
extern const char ove_zh_reserved_heap_link_anchor_;
__attribute__((used)) static const void *const ove_zh_reserved_heap_link_ref_ =
	&ove_zh_reserved_heap_link_anchor_;
#endif

/* ── Libc malloc-family wrappers ─────────────────────────────────────
 *
 * These satisfy `--wrap=malloc/calloc/realloc/free/zalloc/memalign`
 * link-time rewrites on every backend.  The `__real_<sym>` forwards
 * are linker-generated when --wrap is in effect; per-backend files
 * provide weak fallbacks that go straight to the underlying kernel
 * allocator (mm_malloc on NuttX, k_malloc on Zephyr, pvPortMalloc on
 * FreeRTOS) so the wrappers are still callable when --wrap isn't
 * actually wired (e.g. NuttX flat-build LDFLAGS that don't propagate
 * to the kernel-level link).
 */

extern void *__real_malloc(size_t n);
extern void *__real_calloc(size_t nmemb, size_t n);
extern void *__real_realloc(void *p, size_t n);
extern void *__real_zalloc(size_t n);
extern void *__real_memalign(size_t alignment, size_t size);
extern void __real_free(void *p);

void *__wrap_malloc(size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_alloc_(n);
	return __real_malloc(n);
}

void *__wrap_calloc(size_t nmemb, size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_calloc_(nmemb, n);
	return __real_calloc(nmemb, n);
}

void *__wrap_realloc(void *p, size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_realloc_(p, n);
	return __real_realloc(p, n);
}

void *__wrap_zalloc(size_t n)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_zalloc_(n);
	return __real_zalloc(n);
}

void *__wrap_memalign(size_t alignment, size_t size)
{
	if (ove_heap_lock_trapped_())
		return ove_heap_lock_post_memalign_(alignment, size);
	return __real_memalign(alignment, size);
}

void __wrap_free(void *p)
{
	/* Pointers allocated from the post-lock pool must go back to it;
	 * the rest go to the main allocator. Free is allowed post-lock —
	 * destruction is fine, only new allocs from the main heap
	 * indicate boot-pattern violations. */
	if (ove_heap_lock_post_free_(p))
		return;
	__real_free(p);
}

#else /* !CONFIG_OVE_ZERO_HEAP */

/* In heap mode the lock has no work to do — provide an empty body so
 * apps can call it unconditionally.  Tests are zero-heap-only so the
 * test-mode hooks live behind the ifdef. */
void ove_heap_lock(void)
{
}

#endif /* CONFIG_OVE_ZERO_HEAP */
