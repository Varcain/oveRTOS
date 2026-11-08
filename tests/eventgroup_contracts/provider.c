/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Inert observers never initialize or destroy a kernel object. */
static unsigned checks, failures, calls, destroys, live, fail_at, bad_entry;
static int init_error;
static void *objects[2];
static const char *platform;

static void check(int value, const char *name)
{
	++checks;
	if (!value) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
#ifndef CONFIG_OVE_RTOS_ZEPHYR
static int initialize(void *object)
{
	unsigned id = ++calls;
	if (id == fail_at)
		return init_error;
	if (id > 2) {
		++bad_entry;
		return EINVAL;
	}
	objects[id - 1] = object;
	++live;
	return 0;
}
static int destroy(void *object)
{
	for (unsigned i = 0; i < 2; ++i) {
		if (objects[i] == object) {
			objects[i] = NULL;
			--live;
			++destroys;
			errno = ENOSPC;
			return 0;
		}
	}
	++bad_entry;
	return EINVAL;
}
#endif
static void reset(unsigned stage, int error)
{
	/* Discard only observer metadata; there are no actual native resources. */
	memset(objects, 0, sizeof(objects));
	calls = destroys = live = bad_entry = 0;
	fail_at = stage;
	init_error = error;
}

#ifdef CONFIG_OVE_RTOS_POSIX
static int tracked_mutex_init(pthread_mutex_t *p, const pthread_mutexattr_t *a)
{
	(void)a;
	return initialize(p);
}
static int tracked_cond_init(pthread_cond_t *p, const pthread_condattr_t *a)
{
	(void)a;
	return initialize(p);
}
static int tracked_mutex_destroy(pthread_mutex_t *p)
{
	return destroy(p);
}
static int tracked_cond_destroy(pthread_cond_t *p)
{
	return destroy(p);
}
#define pthread_mutex_init tracked_mutex_init
#define pthread_cond_init tracked_cond_init
#define pthread_mutex_destroy tracked_mutex_destroy
#define pthread_cond_destroy tracked_cond_destroy
#define NATIVE_STAGES 2u
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
#include <event_groups.h>
void model_enter_critical(void)
{
}
void model_exit_critical(void)
{
}
UBaseType_t model_enter_critical_from_isr(void)
{
	return 0;
}
void model_exit_critical_from_isr(UBaseType_t key)
{
	(void)key;
}
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *p)
{
	return initialize(p) ? NULL : p;
}
void vEventGroupDelete(EventGroupHandle_t p)
{
	(void)destroy(p);
}
#define NATIVE_STAGES 1u
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/semaphore.h>
int nxsem_init(sem_t *p, int shared, int32_t value)
{
	if (shared != 0 || value != 0)
		++bad_entry;
	return -initialize(p);
}
int nxsem_destroy(sem_t *p)
{
	return -destroy(p);
}
clock_t clock_systime_ticks(void);
bool clock_compare(clock_t, clock_t);
#define NATIVE_STAGES 1u
#else
#include <zephyr/kernel.h>
void k_event_init(struct k_event *p)
{
	++calls;
	p->opaque = 0;
	p->events = 0;
	p->lock.opaque = 0;
}
#define NATIVE_STAGES 0u
#endif

#include OVE_EVENTGROUP_PROVIDER

#ifndef CONFIG_OVE_ZERO_HEAP
static void *allocation;
static unsigned allocated, freed;
static bool fail_allocation;
static void *observed_malloc(size_t size)
{
	++allocated;
	if (fail_allocation)
		return NULL;
	assert(allocation == NULL);
	allocation = malloc(size);
	return allocation;
}
static void observed_free(void *p)
{
	check(p != NULL && p == allocation, "common helper frees its own allocation");
	++freed;
	allocation = NULL;
	/* Remove inert resource tokens before ending the real buffer's lifetime. */
	memset(objects, 0, sizeof(objects));
	free(p);
}
#undef OVE_BACKEND_MALLOC
#undef OVE_BACKEND_FREE
#define OVE_BACKEND_MALLOC(size) observed_malloc(size)
#define OVE_BACKEND_FREE(p) observed_free(p)
#include "../../src/ove_eventgroup.c"
static void cleanup_observation(void)
{
	/* A failing subject cannot make the observer itself leak its byte buffer. */
	memset(objects, 0, sizeof(objects));
	free(allocation);
	allocation = NULL;
	allocated = freed = 0;
	fail_allocation = false;
}
#endif

static void construction(void)
{
	ove_eventgroup_storage_t backing, sentinel;
	ove_eventgroup_t handle = &sentinel;
	reset(0, ENOMEM);
	check(ove_eventgroup_init(NULL, &backing) == OVE_ERR_INVALID_PARAM && calls == 0,
	      "null output rejected before native entry");
	check(ove_eventgroup_init(&handle, NULL) == OVE_ERR_INVALID_PARAM && handle == &sentinel &&
		      calls == 0,
	      "null storage preserves output and avoids native entry");
	for (unsigned stage = 1; stage <= NATIVE_STAGES; ++stage) {
		const int errors[] = {ENOMEM, EINVAL, EIO};
#ifndef CONFIG_OVE_RTOS_FREERTOS
		const int results[] = {OVE_ERR_NO_MEMORY, OVE_ERR_INVALID_PARAM, OVE_ERR_IO};
#endif
		for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
			reset(stage, errors[i]);
			handle = &sentinel;
			errno = ENOSPC;
			int rc = ove_eventgroup_init(&handle, &backing);
#ifdef CONFIG_OVE_RTOS_FREERTOS
			int wanted = OVE_ERR_NO_MEMORY;
#else
			int wanted = results[i];
#endif
			check(rc == wanted, "native initialization error preserved");
			check(handle == &sentinel, "failure does not publish a handle");
			check(calls == stage, "failure stops subsequent initialization");
			check(live == 0 && destroys == stage - 1 && bad_entry == 0,
			      "only successfully initialized resources are cleaned");
			reset(0, 0);
			handle = NULL;
			rc = ove_eventgroup_init(&handle, &backing);
			check(rc == OVE_OK && handle == &backing,
			      "same backing retries after failure");
			if (rc == OVE_OK && handle == &backing)
				ove_eventgroup_deinit(handle);
			check(live == 0 && bad_entry == 0,
			      "retried group completes native cleanup");
		}
	}
	reset(0, 0);
	handle = NULL;
	int rc = ove_eventgroup_init(&handle, &backing);
	check(rc == OVE_OK && handle == &backing && calls == (NATIVE_STAGES ? NATIVE_STAGES : 1),
	      "valid initialization publishes only after native stages");
	check(backing.notify_cb == NULL && backing.notify_ud == NULL,
	      "new group starts without a callback");
	if (rc == OVE_OK && handle == &backing)
		ove_eventgroup_deinit(handle);
	check(live == 0 && bad_entry == 0, "valid group completes native cleanup");
}

#ifndef CONFIG_OVE_ZERO_HEAP
static void heap_cleanup(void)
{
	ove_eventgroup_storage_t sentinel;
	ove_eventgroup_t handle = &sentinel;
	reset(0, ENOMEM);
	fail_allocation = true;
	check(ove_eventgroup_create(&handle) == OVE_ERR_NO_MEMORY && calls == 0 &&
		      handle == &sentinel && allocated == 1 && freed == 0,
	      "allocation failure avoids initialization and publication");
	cleanup_observation();
	for (unsigned stage = 1; stage <= NATIVE_STAGES; ++stage) {
		reset(stage, ENOMEM);
		handle = &sentinel;
		int rc = ove_eventgroup_create(&handle);
		check(rc == OVE_ERR_NO_MEMORY && handle == &sentinel,
		      "common creation propagates unpublished initialization failure");
		check(allocated == 1 && freed == 1 && allocation == NULL,
		      "common helper releases failed initialization allocation");
		check(calls == stage && destroys == stage - 1 && live == 0 && bad_entry == 0,
		      "common failure keeps exact native cleanup");
		cleanup_observation();
	}
	reset(0, 0);
	handle = NULL;
	int rc = ove_eventgroup_create(&handle);
	check(rc == OVE_OK && handle != NULL && allocated == 1 && freed == 0,
	      "common creation owns a successful allocation");
	if (rc == OVE_OK && handle != NULL)
		ove_eventgroup_destroy(handle);
	check(freed == 1 && allocation == NULL && live == 0 && bad_entry == 0,
	      "common destruction completes cleanup before release");
	cleanup_observation();
}
#endif

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
#ifdef CONFIG_OVE_RTOS_POSIX
	platform = "posix";
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	platform = "freertos-declared-null";
#elif defined(CONFIG_OVE_RTOS_NUTTX)
	platform = "nuttx-declared-error";
#else
	platform = "zephyr-void-init";
#endif
	if (argc == 2)
		check(0, "early failure survives controls");
	construction();
#ifndef CONFIG_OVE_ZERO_HEAP
	heap_cleanup();
#endif
	printf("eventgroup provider %s: checks=%u failures=%u\n", platform, checks, failures);
	return failures != 0;
}
