/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native provider faults are no-effect, single-shot observations. They do not
 * claim that the actual kernels produce these errors for valid backing. */
static unsigned failures, controls, interventions;
static struct {
	ove_mutex_t mutex;
	ove_condvar_t cv;
	unsigned waits, acquisitions, releases, guards, guard_releases, injections;
	bool active, signaled, release_error, acquire_error, entry_error, retire_error;
	int wait_error;
#ifdef CONFIG_OVE_RTOS_POSIX
	unsigned depth;
	bool synthetic;
#endif
} observed;

static void check(bool condition, const char *message)
{
	if (!condition) {
		++failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

static void reject(void)
{
	fputs("fixture rejected: invalid condition observer setup\n", stderr);
	exit(2);
}

#ifndef CONFIG_OVE_RTOS_POSIX
static bool inject(bool *armed)
{
	if (!observed.active || !*armed)
		return false;
	*armed = false;
	++observed.injections;
	return true;
}
#endif

#ifdef CONFIG_OVE_RTOS_FREERTOS
struct model_task {
	unsigned live;
};
static struct model_task current = {1};
static unsigned critical_depth;

void model_enter_critical(void)
{
	++critical_depth;
}
void model_exit_critical(void)
{
	if (!critical_depth)
		reject();
	--critical_depth;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &current;
}
TickType_t xTaskGetTickCount(void)
{
	return 1;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
	*storage = (StaticSemaphore_t){.initialized = true};
	return storage;
}
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
	if (!sem->initialized || sem->depth)
		reject();
	sem->initialized = false;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
	if (!sem->initialized || sem->depth != 1 || sem->owner != &current)
		reject();
	if (observed.active) {
		++observed.releases;
		if (inject(&observed.release_error))
			return pdFALSE;
	}
	sem->depth = 0;
	sem->owner = NULL;
	return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
	(void)ticks;
	if (!sem->initialized)
		reject();
	if (observed.active) {
		++observed.acquisitions;
		/* This hook runs while the adapter's waiter frame is still live. */
		if (observed.cv->head != NULL) {
			check(false, "waiter retired before caller reacquisition");
			observed.cv->head = NULL;
			++interventions;
		}
		if (inject(&observed.acquire_error))
			return pdFALSE;
	}
	if (sem->depth) {
		check(false, "caller acquisition follows successful release");
		++interventions;
		return pdFALSE;
	}
	sem->depth = 1;
	sem->owner = &current;
	return pdTRUE;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	(void)clear;
	(void)ticks;
	if (!observed.active || critical_depth)
		reject();
	++observed.waits;
	check(observed.mutex->sem->depth == 0, "wait follows successful release");
	return observed.signaled;
}
static unsigned depth(void)
{
	return observed.mutex->sem->depth;
}
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
static struct k_thread current;
int k_mutex_init(struct k_mutex *mutex)
{
	*mutex = (struct k_mutex){.initialized = true};
	return 0;
}
int k_mutex_unlock(struct k_mutex *mutex)
{
	if (!mutex->initialized || mutex->lock_count == 0 || mutex->owner != &current)
		reject();
	if (observed.active)
		++observed.releases;
	if (--mutex->lock_count == 0)
		mutex->owner = NULL;
	return 0;
}
int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)timeout;
	if (!mutex->initialized)
		reject();
	if (observed.active) {
		++observed.acquisitions;
		if (inject(&observed.acquire_error))
			return -EIO;
	}
	/* Zephyr accepts native recursion: count it so an extra adapter take
	 * after successful native reacquisition is a defined, visible failure. */
	++mutex->lock_count;
	mutex->owner = &current;
	return 0;
}
int k_condvar_init(struct k_condvar *cv)
{
	*cv = (struct k_condvar){.initialized = true};
	return 0;
}
int k_condvar_wait(struct k_condvar *cv, struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)timeout;
	if (!cv->initialized || !observed.active)
		reject();
	++observed.waits;
	if (k_mutex_unlock(mutex))
		reject();
	if (observed.wait_error) {
		++observed.injections;
		return observed.wait_error;
	}
	if (observed.signaled) {
		if (k_mutex_lock(mutex, K_FOREVER))
			reject();
		return 0;
	}
	return -EAGAIN;
}
static unsigned depth(void)
{
	return observed.mutex->mtx.lock_count;
}
#elif defined(CONFIG_OVE_RTOS_NUTTX)
int nxmutex_init(mutex_t *mutex)
{
	*mutex = (mutex_t){.initialized = true};
	return 0;
}
int nxmutex_destroy(mutex_t *mutex)
{
	if (!mutex->initialized || mutex->held)
		reject();
	mutex->initialized = false;
	return 0;
}
int nxmutex_lock(mutex_t *mutex)
{
	if (!mutex->initialized)
		reject();
	if (observed.active && mutex == &observed.mutex->mtx) {
		++observed.acquisitions;
		if (inject(&observed.acquire_error))
			return -EIO;
	} else if (observed.active && mutex == &observed.cv->guard) {
		++observed.guards;
		if (inject(observed.guard_releases ? &observed.retire_error
						   : &observed.entry_error))
			return -EIO;
	}
	if (mutex->held) {
		check(false, "caller acquisition follows successful release");
		++interventions;
		return -EDEADLK;
	}
	mutex->held = 1;
	return 0;
}
int nxmutex_ticklock(mutex_t *mutex, uint32_t ticks)
{
	(void)ticks;
	return nxmutex_lock(mutex);
}
int nxmutex_unlock(mutex_t *mutex)
{
	if (!mutex->initialized || mutex->held != 1)
		reject();
	if (observed.active && mutex == &observed.mutex->mtx) {
		++observed.releases;
		if (inject(&observed.release_error))
			return -EIO;
	} else if (observed.active && mutex == &observed.cv->guard) {
		++observed.guard_releases;
	}
	mutex->held = 0;
	return 0;
}
int nxsem_init(sem_t *sem, int shared, int32_t count)
{
	(void)shared;
	*sem = (sem_t){.initialized = true, .count = count};
	return 0;
}
int nxsem_destroy(sem_t *sem)
{
	if (!sem->initialized)
		reject();
	sem->initialized = false;
	return 0;
}
int nxsem_wait_uninterruptible(sem_t *sem)
{
	if (!sem->initialized || !observed.active)
		reject();
	++observed.waits;
	if (observed.mutex->mtx.held) {
		check(false, "wait follows successful release");
		++interventions;
		return -EIO;
	}
	if (observed.wait_error) {
		++observed.injections;
		return observed.wait_error;
	}
	return observed.signaled ? 0 : -ETIMEDOUT;
}
int nxsem_tickwait_uninterruptible(sem_t *sem, uint32_t ticks)
{
	(void)ticks;
	return nxsem_wait_uninterruptible(sem);
}
static unsigned depth(void)
{
	return observed.mutex->mtx.held;
}
#else
#include <pthread.h>
static int model_mutex_lock(pthread_mutex_t *mutex)
{
	int result = pthread_mutex_lock(mutex);
	if (result)
		reject();
	++observed.depth;
	if (observed.active)
		++observed.acquisitions;
	return result;
}
static int model_mutex_unlock(pthread_mutex_t *mutex)
{
	if (observed.depth != 1)
		reject();
	int result = pthread_mutex_unlock(mutex);
	if (result)
		reject();
	--observed.depth;
	if (observed.active)
		++observed.releases;
	return result;
}
static int model_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex)
{
	(void)cv;
	(void)mutex;
	if (!observed.active || observed.depth != 1 || !observed.synthetic)
		reject();
	++observed.waits;
	if (observed.wait_error)
		++observed.injections;
	return observed.wait_error;
}
static int model_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex,
				const struct timespec *until)
{
	if (observed.synthetic)
		return model_cond_wait(cv, mutex);
	if (!observed.active || observed.depth != 1)
		reject();
	++observed.waits;
	return pthread_cond_timedwait(cv, mutex, until);
}
void ove_backend_thread_set_state(int state)
{
	(void)state;
}
static unsigned depth(void)
{
	return observed.depth;
}
#define pthread_mutex_lock model_mutex_lock
#define pthread_mutex_unlock model_mutex_unlock
#define pthread_cond_wait model_cond_wait
#define pthread_cond_timedwait model_cond_timedwait
#endif

#include OVE_SYNC_BACKEND_SOURCE

static int checked_wait(ove_condvar_t cv, ove_mutex_t mutex, uint64_t timeout, bool *held)
{
#ifdef OVE_TEST_LEGACY_OUTCOME
	/* Compatibility witness for the status-only parent, never a native ABI. */
	if (held == NULL)
		return OVE_ERR_INVALID_PARAM;
	*held = mutex != NULL;
	if (cv == NULL || mutex == NULL)
		return OVE_ERR_INVALID_PARAM;
	int result = ove_condvar_wait(cv, mutex, timeout);
	*held = result == OVE_OK || result == OVE_ERR_TIMEOUT;
	return result;
#else
	return ove_condvar_wait_checked(cv, mutex, timeout, held);
#endif
}

static void one_case(const char *name, bool control)
{
	ove_mutex_storage_t mutex_storage = {0};
	ove_condvar_storage_t cv_storage = {0};
	observed = (__typeof__(observed)){0};
	if (ove_mutex_init(&observed.mutex, &mutex_storage) ||
	    ove_condvar_init(&observed.cv, &cv_storage) ||
	    ove_mutex_lock(observed.mutex, OVE_WAIT_FOREVER))
		reject();
	observed.active = true;
	observed.signaled = !strcmp(name, "success");
	observed.release_error = !strcmp(name, "release_error");
	observed.acquire_error = !strcmp(name, "acquire_error") || !strcmp(name, "combined_error");
	observed.entry_error = !strcmp(name, "entry_error");
	observed.retire_error = !strcmp(name, "retire_error") || !strcmp(name, "combined_error");
	if (!strcmp(name, "wait_error"))
		observed.wait_error = -EIO;
#ifdef CONFIG_OVE_RTOS_POSIX
	observed.synthetic = strcmp(name, "timeout") != 0;
	if (!strcmp(name, "wait_error"))
		observed.wait_error = EINVAL;
	if (!strcmp(name, "eagain_error"))
		observed.wait_error = EAGAIN;
#endif
	int expected = observed.signaled ? OVE_OK : OVE_ERR_TIMEOUT;
	if (observed.release_error || observed.acquire_error || observed.entry_error ||
	    observed.retire_error || observed.wait_error)
		expected = OVE_ERR_IO;
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if (expected == OVE_ERR_IO)
		expected = OVE_ERR_BUSY;
#elif defined(CONFIG_OVE_RTOS_POSIX)
	if (expected == OVE_ERR_IO)
		expected = OVE_ERR_INVALID_PARAM;
	if (!strcmp(name, "eagain_error"))
		expected = OVE_ERR_BUSY;
#endif
	bool held = !observed.signaled;
	int result = checked_wait(observed.cv, observed.mutex, 0, &held);
	unsigned physical = depth();
	check(held == (physical == 1), "reported ownership matches provider");
	check(result == expected, "checked wait preserves return class");
	bool expected_held = strcmp(name, "acquire_error") && strcmp(name, "combined_error");
#ifdef CONFIG_OVE_RTOS_ZEPHYR
	if (!strcmp(name, "wait_error"))
		expected_held = false;
#endif
	check(physical == (unsigned)expected_held, "provider follows selected transition");
	unsigned registrations = 0;
#ifdef CONFIG_OVE_RTOS_NUTTX
	registrations = (unsigned)observed.cv->nwaiters;
	check(registrations == 0, "waiter registration retired before return");
#endif
	if (!control)
		printf("outcome: status=%d held=%u physical=%u waits=%u takes=%u gives=%u "
		       "guards=%u guard_releases=%u injections=%u registrations=%u\n",
		       result, held, physical, observed.waits, observed.acquisitions,
		       observed.releases, observed.guards, observed.guard_releases,
		       observed.injections, registrations);
	/* Cleanup depends on independent provider state, never the output under
	 * test. Faults are disarmed before any real release or destruction. */
	observed.active = false;
#ifdef CONFIG_OVE_RTOS_NUTTX
	if (registrations) {
		if (nxmutex_lock(&observed.cv->guard))
			reject();
		observed.cv->nwaiters = 0;
		if (nxmutex_unlock(&observed.cv->guard))
			reject();
		++interventions;
	}
#endif
	for (unsigned remaining = physical; remaining; --remaining)
		ove_mutex_unlock(observed.mutex);
	ove_condvar_deinit(observed.cv);
	ove_mutex_deinit(observed.mutex);
	if (control)
		++controls;
}

static void null_cases(void)
{
	ove_mutex_storage_t mutex_storage = {0};
	ove_condvar_storage_t cv_storage = {0};
	observed = (__typeof__(observed)){0};
	if (ove_mutex_init(&observed.mutex, &mutex_storage) ||
	    ove_condvar_init(&observed.cv, &cv_storage) ||
	    ove_mutex_lock(observed.mutex, OVE_WAIT_FOREVER))
		reject();
	observed.active = true;
	bool held = false;
	check(checked_wait(NULL, observed.mutex, 0, &held) == OVE_ERR_INVALID_PARAM && held,
	      "null condition preserves held entry ownership");
	check(checked_wait(observed.cv, NULL, 0, &held) == OVE_ERR_INVALID_PARAM && !held,
	      "null mutex initializes absent ownership");
	check(checked_wait(observed.cv, observed.mutex, 0, NULL) == OVE_ERR_INVALID_PARAM,
	      "null outcome is rejected without dispatch");
	check(observed.waits == 0 && observed.acquisitions == 0 && observed.releases == 0 &&
		      observed.guards == 0 && depth() == 1,
	      "invalid arguments have no provider effects");
	observed.active = false;
	ove_mutex_unlock(observed.mutex);
	ove_condvar_deinit(observed.cv);
	ove_mutex_deinit(observed.mutex);
	puts("outcome: nulls=3 dispatches=0");
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const char *name = argv[1];
	bool known = !strcmp(name, "success") || !strcmp(name, "timeout") ||
		     !strcmp(name, "canary") || !strcmp(name, "nulls");
#if defined(CONFIG_OVE_RTOS_FREERTOS) || defined(CONFIG_OVE_RTOS_NUTTX)
	known |= !strcmp(name, "release_error");
#endif
#ifndef CONFIG_OVE_RTOS_POSIX
	known |= !strcmp(name, "acquire_error");
#endif
#ifndef CONFIG_OVE_RTOS_FREERTOS
	known |= !strcmp(name, "wait_error");
#endif
#ifdef CONFIG_OVE_RTOS_NUTTX
	known |= !strcmp(name, "entry_error") || !strcmp(name, "retire_error") ||
		 !strcmp(name, "combined_error");
#elif defined(CONFIG_OVE_RTOS_POSIX)
	known |= !strcmp(name, "eagain_error");
#endif
	if (!known)
		return 2;
	if (!strcmp(name, "canary"))
		check(false, "explicit early failure canary");
	one_case("success", true);
	one_case("timeout", true);
	if (!strcmp(name, "nulls"))
		null_cases();
	else
		one_case(!strcmp(name, "canary") ? "timeout" : name, false);
	check(interventions == 0, "ordinary fixture needs no safety intervention");
	printf("%s: failures=%u controls=%u safety=%u\n", name, failures, controls, interventions);
	return failures ? 1 : 0;
}
