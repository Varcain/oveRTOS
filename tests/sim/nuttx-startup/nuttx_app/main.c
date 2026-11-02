/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ove/storage.h>
#include <ove/thread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "framework/semihosting_exit.h"

void lib_cxx_initialize(void);

static atomic_uint constructor_calls;
static atomic_uint tail_calls;
static atomic_uint entry_calls;
static ove_thread_t child;
static ove_thread_storage_t child_storage;
OVE_THREAD_STACK_DEFINE_(child_stack, 8192);
static int startup_status = OVE_ERR_NOT_SUPPORTED;

static void child_entry(void *unused)
{
	(void)unused;
	lib_cxx_initialize();
	atomic_fetch_add(&entry_calls, 1);
}

static void __attribute__((constructor(201))) first_constructor(void)
{
	/* A duplicate callback must return before a repeated init can write
	 * the same storage or color the already active caller-owned stack. */
	if (atomic_fetch_add(&constructor_calls, 1) != 0)
		return;
	startup_status = ove_thread_init(&child, &child_storage, "ctor_child", child_entry, NULL,
					 OVE_PRIO_NORMAL, sizeof(child_stack), child_stack);
}

static void __attribute__((constructor(202))) tail_constructor(void)
{
	atomic_fetch_add(&tail_calls, 1);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	int retired = startup_status;
	if (startup_status == OVE_OK)
		retired = ove_thread_deinit(child);
	lib_cxx_initialize();
	unsigned int first = atomic_load(&constructor_calls);
	unsigned int tail = atomic_load(&tail_calls);
	unsigned int entries = atomic_load(&entry_calls);
	printf("CONSTRUCTOR_COUNTS first=%u tail=%u entries=%u start=%d retire=%d\n", first, tail,
	       entries, startup_status, retired);
	int failed = first != 1 || tail != 1 || entries != 1 || startup_status != OVE_OK ||
		     retired != OVE_OK;
	puts(failed ? "CONSTRUCTOR_STARTUP_FAILED" : "CONSTRUCTORS_ONCE");
	semihosting_exit(failed);
	return failed;
}
