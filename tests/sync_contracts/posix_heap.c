/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define main static_storage_fixture_main
#include "posix.c"
#undef main

#ifndef OVE_HEAP_SYNC
#error "heap forwarding observer requires heap synchronization"
#endif

static void *allocation;
static unsigned allocations, allocation_attempts, free_requests, retained_live;
static unsigned actual_frees, total_free_requests;
static int fail_allocation;

static void *record_malloc(size_t size)
{
	if (allocation)
		reject();
	++allocation_attempts;
	if (fail_allocation) {
		fail_allocation = 0;
		return NULL;
	}
	allocation = malloc(size);
	if (!allocation)
		reject();
	memset(allocation, 0, size);
	++allocations;
	return allocation;
}

static void record_free(void *pointer)
{
	if (!allocation || pointer != allocation || free_requests)
		reject();
	++free_requests;
	++total_free_requests;
	retained_live += live_count() != 0;
	/* Observe the common free request, but retain complete live backing until
	 * the caller clears all typed snapshots and privately releases resources. */
}

#define malloc record_malloc
#define free record_free
#include OVE_COMMON_SYNC_SOURCE
#undef malloc
#undef free

static int create(enum family family, union handle *out)
{
	switch (family) {
	case NORMAL:
		return ove_mutex_create(&out->mutex);
	case RECURSIVE:
		return ove_recursive_mutex_create(&out->mutex);
	case SEMAPHORE:
		return ove_sem_create(&out->sem, 1, 2);
	case EVENT:
		return ove_event_create(&out->event);
	case CONDVAR:
		return ove_condvar_create(&out->cv);
	}
	reject();
	return OVE_ERR_INVALID_PARAM;
}

static void destroy(enum family family, union handle out)
{
	switch (family) {
	case NORMAL:
		ove_mutex_destroy(out.mutex);
		break;
	case RECURSIVE:
		ove_recursive_mutex_destroy(out.mutex);
		break;
	case SEMAPHORE:
		ove_sem_destroy(out.sem);
		break;
	case EVENT:
		ove_event_destroy(out.event);
		break;
	case CONDVAR:
		ove_condvar_destroy(out.cv);
		break;
	}
}

static void release_allocation(void)
{
	if (!allocation || live_count() || attr_live)
		reject();
	void *owned = allocation;
	allocation = NULL;
	free(owned);
	++actual_frees;
	free_requests = 0;
}

static void heap_controls(void)
{
	for (enum family family = NORMAL; family <= CONDVAR; ++family) {
		union handle out = {0};
		if (create(family, &out))
			reject();
		if (family == NORMAL || family == RECURSIVE) {
			int rc = family == NORMAL ? ove_mutex_lock(out.mutex, 0)
						  : ove_recursive_mutex_lock(out.mutex, 0);
			if (rc)
				reject();
			if (family == NORMAL)
				ove_mutex_unlock(out.mutex);
			else
				ove_recursive_mutex_unlock(out.mutex);
		} else if (family == SEMAPHORE) {
			if (ove_sem_take(out.sem, 0))
				reject();
		} else if (family == EVENT) {
			ove_event_signal(out.event);
			if (ove_event_wait(out.event, 0))
				reject();
		} else {
			ove_condvar_signal(out.cv);
		}
		destroy(family, out);
		out = (union handle){0};
		check(free_requests == 1 && live_count() == 0,
		      "heap controls release native pieces before free request");
		release_allocation();
		++controls;
	}
	check(actual_frees == allocations, "every actual heap allocation is released once");
}

static int heap_finish(const char *name)
{
	printf("%s: failures=%u controls=%u injected=%u allocations=%u attempts=%u free_requests=%u actual_frees=%u native_inits=%u destroys=%u guard_interventions=%u retained_live=%u\n",
	       name, failures, controls, injected, allocations, allocation_attempts,
	       total_free_requests, actual_frees, native_inits, native_destroys, interventions,
	       retained_live);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const char *name = argv[1];
	static const char *names[] = {"mutex_init",
				      "recursive_attr_init",
				      "recursive_attr_type",
				      "recursive_mutex_init",
				      "sem_init",
				      "event_mutex_init",
				      "event_cond_init",
				      "condvar_init",
				      "allocation_mutex",
				      "allocation_recursive",
				      "allocation_sem",
				      "allocation_event",
				      "allocation_condvar",
				      "count_zero",
				      "count_order",
				      "count_initial_domain",
				      "count_max_domain",
				      "count_initial_ceiling_plus_one",
				      "count_max_ceiling_plus_one",
				      "controls",
				      "canary"};
	int selected = -1;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (!strcmp(name, names[i]))
			selected = (int)i;
	if (selected < 0)
		return 2;
	if (!strcmp(name, "canary"))
		check(0, "explicit early failure canary");
	ordinary_controls();
	heap_controls();
	if (selected >= 8 && selected <= 18) {
		enum family family = selected <= 12 ? (enum family)(selected - 8) : SEMAPHORE;
		union backing sentinel = {0};
		union handle original = sentinel_handle(family, &sentinel), out = original;
		unsigned entries = native_inits, attempts = allocation_attempts;
		int rc;
		if (selected <= 12) {
			fail_allocation = 1;
			rc = create(family, &out);
			check(rc == OVE_ERR_NO_MEMORY, "common allocation failure is reported");
			check(!allocation && free_requests == 0,
			      "allocation failure owns no storage and requests no free");
		} else {
			unsigned initial = selected == 14 ? 2 : selected == 15 ? UINT_MAX : 0;
			unsigned max = selected == 13 ? 0 : selected == 14 ? 1 : UINT_MAX;
			if (selected >= 17) {
				max = (unsigned)SEM_VALUE_MAX + 1u;
				initial = selected == 17 ? max : 0;
			}
			rc = ove_sem_create(&out.sem, initial, max);
			check(rc == OVE_ERR_INVALID_PARAM, "common heap rejects invalid counts");
			check(free_requests == 1, "invalid counts release the common allocation");
		}
		check(same_handle(family, original, out), "common failure preserves output");
		check(native_inits == entries,
		      "common rejection never enters native initialization");
		check(allocation_attempts == attempts + 1, "common create attempts one allocation");
		out = (union handle){0};
		private_cleanup();
		if (allocation)
			release_allocation();
		heap_controls();
		return heap_finish(name);
	}
	enum family family;
	const char *site;
	int expected;
	if (!strcmp(name, "mutex_init")) {
		family = NORMAL;
		site = "mutex_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "recursive_attr_init")) {
		family = RECURSIVE;
		site = "attr_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "recursive_attr_type")) {
		family = RECURSIVE;
		site = "attr_settype";
		expected = OVE_ERR_INVALID_PARAM;
	} else if (!strcmp(name, "recursive_mutex_init")) {
		family = RECURSIVE;
		site = "mutex_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "sem_init")) {
		family = SEMAPHORE;
		site = "sem_init";
		expected = OVE_ERR_NO_SPACE;
	} else if (!strcmp(name, "event_mutex_init")) {
		family = EVENT;
		site = "mutex_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "event_cond_init")) {
		family = EVENT;
		site = "cond_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "condvar_init")) {
		family = CONDVAR;
		site = "cond_init";
		expected = OVE_ERR_NO_MEMORY;
	} else if (!strcmp(name, "controls") || !strcmp(name, "canary")) {
		return heap_finish(name);
	} else {
		return 2;
	}
	union backing sentinel = {0};
	union handle original = sentinel_handle(family, &sentinel), out = original;
	failure_site = site;
	int rc = create(family, &out);
	check(injected == 1, "heap constructor error injection reached once");
	check(rc == expected, "heap constructor preserves native failure");
	check(same_handle(family, out, original), "failed heap constructor preserves output");
	check(live_count() == 0, "heap error releases native pieces before storage free");
	check(free_requests == 1, "common heap failure requests storage free once");
	check(retained_live == 0, "common heap free never precedes native cleanup");
	out = (union handle){0};
	private_cleanup();
	release_allocation();
	failure_site = NULL;
	/* Real common create/operate/destroy remains usable after the failure. */
	heap_controls();
	return heap_finish(name);
}
