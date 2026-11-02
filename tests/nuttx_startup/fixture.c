/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <nuttx/arch.h>
#include <stdio.h>
#include <string.h>
#include "libc.h"

static unsigned int constructor_calls;
static unsigned int tail_calls;
static unsigned int sequence;
static unsigned int first_order;
static unsigned int tail_order;
static int recursive;

static void first_constructor(void)
{
	++constructor_calls;
	first_order = ++sequence;
	/* A finite duplicate pass reports counts without touching native storage. */
	if (recursive && constructor_calls == 1)
		lib_cxx_initialize();
}

static void tail_constructor(void)
{
	++tail_calls;
	tail_order = ++sequence;
}

initializer_t fixture_initializers[3] = {first_constructor, 0, tail_constructor};
unsigned int fixture_initializer_count = 3;

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	recursive = strcmp(argv[1], "recursive") == 0;
	int empty = strcmp(argv[1], "empty") == 0;
	int nulls = strcmp(argv[1], "nulls") == 0;
	if (!recursive && !empty && !nulls && strcmp(argv[1], "acyclic") != 0)
		return 2;
	if (empty)
		fixture_initializer_count = 0;
	if (nulls)
		for (unsigned int i = 0; i < 3; ++i)
			fixture_initializers[i] = NULL;
	lib_cxx_initialize();
	lib_cxx_initialize();
	printf("CONSTRUCTOR_COUNTS first=%u tail=%u\n", constructor_calls, tail_calls);
	int failed = empty || nulls ? constructor_calls != 0 || tail_calls != 0
				    : constructor_calls != 1 || tail_calls != 1 ||
					      first_order != 1 || tail_order != 2;
	puts(failed ? "CONSTRUCTOR_COUNT_OR_ORDER_FAILED" : "CONSTRUCTORS_ONCE");
	return failed;
}
