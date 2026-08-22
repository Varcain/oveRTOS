/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Phase-0 integration: the three reusable layers composing into a sandboxed
 * module runtime. An arena hands the loader a region from a pool; the loaded
 * module's code is then run inside a fault-contained protected task.
 */

#include "../framework/ove_test.h"
#include "ove/arena.h"
#include "ove/loader.h"
#include "ove/protected.h"

#include <string.h>
#include <sys/mman.h>

#include "loader_mod_image.h"

#define POOL_BYTES (128u * 1024u)
#define REGION_BYTES (16u * 1024u)

/* Symbol the test module imports. */
static long host_mul(long a, long b)
{
	return a * b;
}

/* Wrappers adapting module functions to the ove_ptask_fn signature. */
struct compute_ctx {
	long (*fn)(long);
	long in;
	long out;
};
static void run_compute(void *arg)
{
	struct compute_ctx *c = (struct compute_ctx *)arg;
	c->out = c->fn(c->in);
}

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
struct store_ctx {
	void (*fn)(long *, long);
	long *target;
	long val;
};
static void run_store(void *arg)
{
	struct store_ctx *c = (struct store_ctx *)arg;
	c->fn(c->target, c->val);
}
#endif /* no address/thread sanitizer */

/* Map a pool, init an arena over it, load the module into an arena region, and
 * flip the pool to read+execute. Returns the pool (NULL on failure) and fills
 * *mod. */
static void *load_into_arena(ove_module_t *mod)
{
	uint8_t *pool =
		mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pool == MAP_FAILED)
		return NULL;

	ove_arena_t arena;
	if (ove_arena_init(&arena, pool, POOL_BYTES) != OVE_OK) {
		munmap(pool, POOL_BYTES);
		return NULL;
	}
	void *region = ove_arena_alloc(&arena, REGION_BYTES);
	if (!region) {
		munmap(pool, POOL_BYTES);
		return NULL;
	}

	ove_loader_sym_t imports[] = {{"host_mul", (void *)host_mul}};
	if (ove_loader_load(mod, ove_loader_test_mod, ove_loader_test_mod_len, region, REGION_BYTES,
			    imports, 1) != OVE_OK) {
		munmap(pool, POOL_BYTES);
		return NULL;
	}
	if (mprotect(pool, POOL_BYTES, PROT_READ | PROT_EXEC) != 0) {
		munmap(pool, POOL_BYTES);
		return NULL;
	}
	return pool;
}

/* arena -> loader -> protected: a module loaded into an arena region runs to
 * completion inside a fault-contained task. */
static void test_sandbox_load_and_run(void **state)
{
	(void)state;
	ove_module_t mod;
	void *pool = load_into_arena(&mod);
	assert_non_null(pool);

	struct compute_ctx cc = {(long (*)(long))ove_loader_sym(&mod, "mod_compute"), 10, 0};
	assert_non_null(cc.fn);

	ove_ptask_result_t r = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(run_compute, &cc, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_OK);
	assert_int_equal(cc.out, 10 * 3 + 7); /* host_mul(10,3) + g_bias */

	munmap(pool, POOL_BYTES);
}

/*
 * Containment composes too: a loaded module that misbehaves is trapped while
 * the supervisor survives. Omitted under ASan (which owns SIGSEGV) and TSan
 * (which cannot unwind through the containment handler's siglongjmp).
 */
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
static void test_sandbox_contains_module_fault(void **state)
{
	(void)state;
	ove_module_t mod;
	void *pool = load_into_arena(&mod);
	assert_non_null(pool);

	void (*store)(long *, long) = (void (*)(long *, long))ove_loader_sym(&mod, "mod_store");
	assert_non_null(store);

	/* The loaded code writing to a valid cell works. */
	long cell = 0;
	struct store_ctx ok = {store, &cell, 99};
	ove_ptask_result_t r = OVE_PTASK_FAULT;
	assert_int_equal(ove_ptask_run(run_store, &ok, &r), OVE_OK);
	assert_int_equal(r, OVE_PTASK_OK);
	assert_int_equal(cell, 99);

	/* The same loaded code writing to a forbidden page is contained. */
	long *forbidden = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert_true(forbidden != MAP_FAILED);
	struct store_ctx bad = {store, forbidden, 1};
	ove_ptask_result_t r2 = OVE_PTASK_OK;
	assert_int_equal(ove_ptask_run(run_store, &bad, &r2), OVE_OK);
	assert_int_equal(r2, OVE_PTASK_FAULT);

	munmap(forbidden, 4096);
	munmap(pool, POOL_BYTES);
}
#endif /* no address/thread sanitizer */

int test_sandbox_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_sandbox_load_and_run),
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
		cmocka_unit_test(test_sandbox_contains_module_fault),
#endif
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
