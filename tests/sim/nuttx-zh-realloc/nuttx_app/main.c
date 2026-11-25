/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <nuttx/config.h>
#include <malloc.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "framework/semihosting_exit.h"
#include "ove/app.h"

#ifndef CONFIG_DEBUG_ASSERTIONS
#error "The zero-heap exhaustion test requires CONFIG_DEBUG_ASSERTIONS"
#endif

void *ove_heap_lock_post_realloc_(void *p, size_t n);
int ove_heap_lock_post_free_(void *p);
void ove_heap_lock_test_begin(void);
int ove_heap_lock_test_end(void);
void *__wrap_malloc(size_t n);
void *__wrap_realloc(void *p, size_t n);
void *__wrap_kmm_realloc(void *p, size_t n);
void __wrap_free(void *p);
int ove_heap_lock_post_owns_(const void *p);

static unsigned char *require(void *p)
{
	if (p == NULL) {
		printf("FAIL: allocation\n");
		fflush(stdout);
		semihosting_exit(2);
	}
	return p;
}

static atomic_int blocks_served;

static int exhaust_pool(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	/* Keep the blocks allocated until the firmware exits. */
	for (int i = 0; i < 64; i++) {
		void *p = __wrap_malloc(1024);
		if (p == NULL)
			return 2;
		if (!ove_heap_lock_post_owns_(p))
			return 3;
		atomic_fetch_add(&blocks_served, 1);
	}
	return 4;
}

/* Move main-heap blocks into the reserved pool, as realloc does once the
 * zero-heap lock is engaged, and check that each copy keeps the old data.
 * Then check that a pool block resized after the test unlock stays in the
 * pool, and that an ordinary main-heap block stays in the main heap.
 * Check that the product lock uses the reserve and asserts on exhaustion. */
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int ok = 1;

	unsigned char *p = require(malloc(16));
	unsigned char *neighbour = require(malloc(64));
	memset(p, 0x11, 16);
	memset(neighbour, 0x5A, 64);
	size_t usable = malloc_size(p);
	unsigned char *grown = require(ove_heap_lock_post_realloc_(p, 256));
	for (size_t i = 0; i < 16; i++)
		ok &= grown[i] == 0x11;
	for (size_t i = usable; i < 256; i++)
		ok &= grown[i] != 0x5A;
	ove_heap_lock_post_free_(grown);
	free(neighbour);

	unsigned char *s = require(malloc(64));
	memset(s, 0x22, 64);
	unsigned char *shrunk = require(ove_heap_lock_post_realloc_(s, 8));
	for (size_t i = 0; i < 8; i++)
		ok &= shrunk[i] == 0x22;
	ove_heap_lock_post_free_(shrunk);

	const char data[] = "realloc data";
	ove_heap_lock_test_begin();
	unsigned char *pooled = __wrap_malloc(sizeof(data));
	ok &= ove_heap_lock_test_end() == 1;
	pooled = require(pooled);
	ok &= ove_heap_lock_post_owns_(pooled) == 1;
	memcpy(pooled, data, sizeof(data));
	pooled = require(__wrap_realloc(pooled, 64));
	ok &= ove_heap_lock_post_owns_(pooled) == 1 && memcmp(pooled, data, sizeof(data)) == 0;
	pooled = require(__wrap_kmm_realloc(pooled, 96));
	ok &= ove_heap_lock_post_owns_(pooled) == 1 && memcmp(pooled, data, sizeof(data)) == 0;
	__wrap_free(pooled);

	unsigned char *ordinary = require(__wrap_malloc(sizeof(data)));
	ok &= ove_heap_lock_post_owns_(ordinary) == 0;
	memcpy(ordinary, data, sizeof(data));
	ordinary = require(__wrap_realloc(ordinary, 64));
	ok &= ove_heap_lock_post_owns_(ordinary) == 0 && memcmp(ordinary, data, sizeof(data)) == 0;
	ordinary = require(__wrap_kmm_realloc(ordinary, 96));
	ok &= ove_heap_lock_post_owns_(ordinary) == 0 && memcmp(ordinary, data, sizeof(data)) == 0;
	__wrap_free(ordinary);

	printf("zero-heap product lock: checking reserved allocation\n");
	fflush(stdout);
	ove_heap_lock();
	void *locked = require(__wrap_malloc(32));
	ok &= ove_heap_lock_post_owns_(locked) == 1;
	__wrap_free(locked);

	/* The lower priority lets the parent enter waitpid before the child exits. */
	pid_t child =
		task_create("pool-exhaustion", CONFIG_INIT_PRIORITY - 1, 8192, exhaust_pool, NULL);
	int status = -1;
	int waited = child > 0 && waitpid(child, &status, 0) == child;
	int served = atomic_load(&blocks_served);
	printf("zero-heap pool exhaustion: served %d, exit %d\n", served,
	       waited && WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	ok &= served > 0 && waited && WIFEXITED(status) && WEXITSTATUS(status) == 1;

	printf("zero-heap realloc: %s\n", ok ? "PASS" : "FAIL");
	fflush(stdout);
	semihosting_exit(!ok);
	return !ok;
}
