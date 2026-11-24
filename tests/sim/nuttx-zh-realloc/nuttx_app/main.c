/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <nuttx/config.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "framework/semihosting_exit.h"

void *ove_heap_lock_post_realloc_(void *p, size_t n);
int ove_heap_lock_post_free_(void *p);

static unsigned char *require(void *p)
{
	if (p == NULL) {
		printf("FAIL: allocation\n");
		fflush(stdout);
		semihosting_exit(2);
	}
	return p;
}

/* Move main-heap blocks into the reserved pool, as realloc does once the
 * zero-heap lock is engaged, and check that each copy keeps the old data. */
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

	printf("zero-heap realloc: %s\n", ok ? "PASS" : "FAIL");
	fflush(stdout);
	semihosting_exit(!ok);
	return !ok;
}
