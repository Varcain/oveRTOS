/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include "framework/semihosting_exit.h"
#include <zephyr/app_memory/app_memdomain.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/libc-hooks.h>

K_APPMEM_PARTITION_DEFINE(result_partition);
struct user_result {
	uint32_t mode, prior, after;
};
K_APP_DMEM(result_partition) static struct user_result result = {0, 0x55, 0x55};
static struct k_mem_domain domain;
static struct k_thread worker;
K_THREAD_STACK_DEFINE(worker_stack, 4096);
static ove_eventgroup_storage_t storage;
static unsigned checks, failures;

static void check(bool value, const char *name)
{
	++checks;
	if (!value) {
		++failures;
		printk("FAIL: %s\n", name);
	}
}
static void user_entry(void *handle, void *results, void *unused)
{
	(void)unused;
	struct user_result *r = results;
	r->mode = k_is_user_context();
	r->prior = ove_eventgroup_clear_bits(handle, 1);
	r->after = ove_eventgroup_get_bits(handle);
}
int main(void)
{
	check(!k_is_user_context(), "setup runs in supervisor context");
	ove_eventgroup_t group = NULL;
	int rc = ove_eventgroup_init(&group, &storage);
	check(rc == OVE_OK && group == &storage, "static native group initializes");
	if (rc != OVE_OK || group == NULL)
		goto finish;
	check(ove_eventgroup_set_bits(group, 3) == 3, "supervisor publishes two bits");
	struct k_mem_partition *partitions[] = {&result_partition, &z_libc_partition};
	rc = k_mem_domain_init(&domain, ARRAY_SIZE(partitions), partitions);
	check(rc == 0, "user result partition initializes");
	if (rc != 0)
		goto finish;
	k_tid_t thread = k_thread_create(&worker, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
					 user_entry, group, &result, NULL, 1, K_USER, K_FOREVER);
	check(thread == &worker, "real user thread is created suspended");
	k_mem_domain_add_thread(&domain, thread);
	k_object_access_grant(&storage.event, thread);
	k_thread_start(thread);
	rc = k_thread_join(thread, K_SECONDS(5));
	check(rc == 0, "user thread completes and joins");
	if (rc != 0) {
		k_thread_abort(thread);
		goto finish;
	}
	check(result.mode == 1, "caller actually runs in user mode");
	check(result.prior == 3, "user clear returns full prior group");
	check(result.after == 2, "authorized user query sees remaining bit");
	check(ove_eventgroup_get_bits(group) == 2, "supervisor observes cleared state");
	ove_eventgroup_deinit(group);
finish:
	printk("eventgroup user: checks=%u failures=%u mode=%u prior=%u after=%u\n", checks,
	       failures, result.mode, result.prior, result.after);
	semihosting_exit(failures ? 1 : 0);
}
