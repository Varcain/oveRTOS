/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/queue.h"
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* These observers model initialization only; they never create kernel objects. */
static unsigned checks, failures, calls, destroys, live, fail_at, bad_entry;
static unsigned destroyed_order;
static void *objects[3];
static const char *platform;

#ifndef DEBUGASSERT
#define DEBUGASSERT(condition) assert(condition)
#endif
#ifndef __ASSERT
#define __ASSERT(condition, ...) assert(condition)
#endif

static void check(int ok, const char *name)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}

static int initialize(void *object)
{
	unsigned id = ++calls;
	if (id == fail_at)
		return ENOMEM;
	if (id > 3) {
		++bad_entry;
		return EINVAL;
	}
	objects[id - 1] = object;
	++live;
	return 0;
}

static int destroy(void *object)
{
	for (unsigned i = 0; i < 3; ++i) {
		if (objects[i] == object) {
			objects[i] = NULL;
			--live;
			++destroys;
			destroyed_order = destroyed_order * 10 + i + 1;
			return 0;
		}
	}
	++bad_entry;
	return EINVAL;
}

static void reset(unsigned stage)
{
	/* No actual native object exists, even after a deliberately broken init. */
	memset(objects, 0, sizeof(objects));
	calls = destroys = live = bad_entry = destroyed_order = 0;
	fail_at = stage;
}

#if defined(CONFIG_OVE_RTOS_POSIX)
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
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
#include "FreeRTOS.h"
_Static_assert(sizeof(UBaseType_t) == 4 && SIZE_MAX > UINT32_MAX,
	       "queue observer requires 32-bit native counts and wider host sizes");
QueueHandle_t xQueueCreateStatic(UBaseType_t n, UBaseType_t size, uint8_t *buffer,
				 StaticQueue_t *storage)
{
	(void)buffer;
	if (n == 0 || size == 0) {
		++bad_entry;
		return NULL;
	}
	return initialize(storage) ? NULL : storage;
}
void vQueueDelete(QueueHandle_t p)
{
	(void)destroy(p);
}
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
BaseType_t xQueueReceiveFromISR(QueueHandle_t, void *, BaseType_t *);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t);
UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t);
UBaseType_t uxQueueGetQueueLength(QueueHandle_t);
#elif defined(CONFIG_OVE_RTOS_NUTTX)
#include <nuttx/semaphore.h>
/* Small model limit permits complete buffers; actual target limits are separate. */
#undef SEM_VALUE_MAX
#define SEM_VALUE_MAX 7
int nxsem_init(sem_t *p, int shared, int32_t value)
{
	(void)shared;
	if (value < 0 || value > SEM_VALUE_MAX) {
		++calls;
		++bad_entry;
		return -EINVAL;
	}
	return -initialize(p);
}
int nxsem_destroy(sem_t *p)
{
	return -destroy(p);
}
int nxsem_trywait(sem_t *);
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
#include <zephyr/kernel.h>
void k_msgq_init(struct k_msgq *p, char *buffer, size_t size, uint32_t n)
{
	(void)buffer;
	(void)size;
	p->max_msgs = n;
	(void)initialize(p);
}
void k_msgq_purge(struct k_msgq *p)
{
	(void)destroy(p);
}
int k_msgq_put(struct k_msgq *, const void *, k_timeout_t);
int k_msgq_get(struct k_msgq *, void *, k_timeout_t);
uint32_t k_msgq_num_used_get(struct k_msgq *);
uint32_t k_msgq_max_msgs_get(struct k_msgq *);
#endif

#include OVE_QUEUE_PROVIDER

static void invalid_dimensions(void)
{
	ove_queue_storage_t storage;
	unsigned char buffer[8];
	ove_queue_t handle = NULL;
	reset(0);
	int rc = ove_queue_init(&handle, &storage, buffer, 0, 2);
	check(rc == OVE_ERR_INVALID_PARAM && calls == 0 && handle == NULL,
	      "zero item size rejected before native entry");
	rc = ove_queue_init(&handle, &storage, buffer, 1, 0);
	check(rc == OVE_ERR_INVALID_PARAM && calls == 0 && handle == NULL,
	      "zero capacity rejected before native entry");
	/* The inert provider cannot access a payload or perform an invalid kernel call. */
	rc = ove_queue_init(&handle, &storage, buffer, SIZE_MAX, 2);
	check(rc == OVE_ERR_INVALID_PARAM && calls == 0 && handle == NULL,
	      "overflow rejected before native entry");
	reset(0);
#ifdef CONFIG_OVE_RTOS_FREERTOS
	/* Host size_t is wider than the modeled 32-bit UBaseType_t. No data is read. */
	handle = NULL;
	rc = ove_queue_init(&handle, &storage, buffer, (size_t)UINT32_MAX + 1, 1);
	check(rc == OVE_ERR_INVALID_PARAM && calls == 0 && handle == NULL,
	      "unrepresentable item size rejected before native entry");
	reset(0);
#endif
#ifdef CONFIG_OVE_RTOS_NUTTX
	handle = NULL;
	rc = ove_queue_init(&handle, &storage, buffer, 1, 8);
	check(rc == OVE_ERR_INVALID_PARAM && calls == 0 && handle == NULL,
	      "provider count ceiling rejected before native entry");
	reset(0);
#endif
}

static void stages(void)
{
	unsigned stages = 0;
#ifdef CONFIG_OVE_RTOS_POSIX
	stages = 3;
#elif defined(CONFIG_OVE_RTOS_NUTTX)
	stages = 2;
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	stages = 1;
#endif
	for (unsigned stage = 1; stage <= stages; ++stage) {
		ove_queue_storage_t storage;
		unsigned char buffer[8];
		ove_queue_t handle = NULL;
		reset(stage);
		errno = EIO;
		int rc = ove_queue_init(&handle, &storage, buffer, 1, 4);
		check(rc == OVE_ERR_NO_MEMORY, "initialization error propagated");
		check(handle == NULL, "failed initialization does not publish handle");
		check(calls == stage, "failure stops later initialization stages");
		check(live == 0 && destroys == stage - 1 && bad_entry == 0,
		      "only earlier initialized resources are cleaned up");
		unsigned order = stage == 3 ? 21 : (stage == 2 ? 1 : 0);
		check(destroyed_order == order, "failure cleanup runs in reverse order");
		reset(0);
		rc = ove_queue_init(&handle, &storage, buffer, 1, 4);
		check(rc == OVE_OK && handle != NULL, "same backing retries after failure");
		if (rc == OVE_OK && handle != NULL)
			ove_queue_deinit(handle);
		check(live == 0 && bad_entry == 0, "retried queue finishes cleanup");
	}
	ove_queue_storage_t storage;
	unsigned char buffer[8];
	ove_queue_t handle = NULL;
	reset(0);
	int rc = ove_queue_init(&handle, &storage, buffer, 1, 4);
	check(rc == OVE_OK && handle != NULL && live != 0, "valid queue initializes");
	if (rc == OVE_OK && handle != NULL)
		ove_queue_deinit(handle);
	check(live == 0 && bad_entry == 0, "valid queue finishes cleanup");
	reset(0);
}

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && strcmp(argv[1], "canary")))
		return 2;
#ifdef CONFIG_OVE_RTOS_POSIX
	platform = "posix";
#elif defined(CONFIG_OVE_RTOS_FREERTOS)
	platform = "freertos";
#elif defined(CONFIG_OVE_RTOS_NUTTX)
	platform = "nuttx-model-limit-7";
#else
	platform = "zephyr";
#endif
	if (argc == 2)
		check(0, "early failure survives controls");
	invalid_dimensions();
	stages();
	printf("queue provider %s: checks=%u failures=%u\n", platform, checks, failures);
	return failures != 0;
}
