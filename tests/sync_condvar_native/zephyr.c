/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "checks.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ove/sync.h>

/* The allocation macro discards its size argument in zero-heap mode. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "ove_backend_common.h"
#pragma GCC diagnostic pop

static struct k_thread current;
static struct {
	int wait_result;
	int lock_result;
	bool within_wait;
	unsigned int adapter_locks;
	unsigned int provider_locks;
	unsigned int waits;
	int64_t last_timeout;
} model;

static void reset_model(void)
{
	memset(&model, 0, sizeof(model));
}

int k_mutex_init(struct k_mutex *mutex)
{
	*mutex = (struct k_mutex){.initialized = true};
	return 0;
}

int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)timeout;
	if (!mutex->initialized || mutex->lock_count >= 8 ||
	    (mutex->owner != NULL && mutex->owner != &current))
		fixture_reject("invalid model mutex acquisition");
	if (model.within_wait)
		++model.provider_locks;
	else
		++model.adapter_locks;
	if (!model.within_wait && model.lock_result)
		return model.lock_result;
	mutex->owner = &current;
	++mutex->lock_count;
	return 0;
}

int k_mutex_unlock(struct k_mutex *mutex)
{
	if (!mutex->initialized || mutex->owner != &current || !mutex->lock_count)
		fixture_reject("invalid model mutex release");
	if (--mutex->lock_count == 0)
		mutex->owner = NULL;
	return 0;
}

int k_condvar_init(struct k_condvar *condvar)
{
	*condvar = (struct k_condvar){.initialized = true};
	return 0;
}

int k_condvar_wait(struct k_condvar *condvar, struct k_mutex *mutex, k_timeout_t timeout)
{
	if (!condvar->initialized || mutex->owner != &current || mutex->lock_count != 1)
		fixture_reject("invalid model condition wait");
	++model.waits;
	model.last_timeout = timeout.ticks;
	(void)k_mutex_unlock(mutex);
	if (model.wait_result == 0) {
		model.within_wait = true;
		(void)k_mutex_lock(mutex, K_FOREVER);
		model.within_wait = false;
	}
	/* Unexpected errors deliberately use one synthetic unheld state. */
	return model.wait_result;
}

int k_condvar_signal(struct k_condvar *condvar)
{
	if (!condvar->initialized)
		fixture_reject("invalid model condition signal");
	return 0;
}

int k_condvar_broadcast(struct k_condvar *condvar)
{
	if (!condvar->initialized)
		fixture_reject("invalid model condition broadcast");
	return 0;
}

/* Include the actual adapter and public storage types in this translation unit. */
#include OVE_SYNC_BACKEND_SOURCE

static void exercise(const char *label, uint64_t timeout, int wait_result, int lock_result,
		     int expected, bool nonclean, bool ordinary_success)
{
	reset_model();
	ove_mutex_storage_t mutex_storage = {0};
	ove_condvar_storage_t cv_storage = {0};
	ove_mutex_t mutex = NULL;
	ove_condvar_t cv = NULL;
	if (ove_mutex_init(&mutex, &mutex_storage) || ove_condvar_init(&cv, &cv_storage) ||
	    ove_mutex_lock(mutex, OVE_WAIT_FOREVER))
		fixture_reject("model setup failed");
	const unsigned int before_locks = model.adapter_locks;
	model.wait_result = wait_result;
	model.lock_result = lock_result;
	const int rc = ove_condvar_wait(cv, mutex, timeout);
	const bool held = mutex_storage.mtx.owner == &current;
	const unsigned int depth = mutex_storage.mtx.lock_count;
	const unsigned int relocks = model.adapter_locks - before_locks;
	(void)printf("observed: %s rc=%d held=%u depth=%u relocks=%u\n", label, rc,
		     (unsigned int)held, depth, relocks);
	if (nonclean)
		fixture_check(rc < 0 && rc != OVE_ERR_TIMEOUT,
			      "native failure is not a clean timeout");
	else
		fixture_check(rc == expected, "native wait result preserved");
	fixture_check(model.waits == 1, "one native condition wait");
	if (expected == OVE_ERR_TIMEOUT && !nonclean) {
		fixture_check(held && depth == 1, "timeout returns held mutex");
		fixture_check(relocks == 1, "timeout reacquires once");
	} else if (ordinary_success) {
		fixture_check(held && depth == 1, "success retains one held level");
		fixture_check(relocks == 0 && model.provider_locks == 1,
			      "successful wake has no adapter relock");
	}
	if (wait_result == -EAGAIN && lock_result)
		fixture_check(relocks == 1, "failed reacquisition was attempted");
	fixture_check(model.last_timeout == (timeout == OVE_WAIT_FOREVER ? -1 : (int64_t)timeout),
		      "native timeout argument preserved");
	/* Snapshot above precedes all repairs; errors promise no held-state result. */
	model.lock_result = 0;
	if (!held && k_mutex_lock(&mutex_storage.mtx, K_FOREVER))
		fixture_reject("model ownership restoration failed");
	while (mutex_storage.mtx.lock_count)
		ove_mutex_unlock(mutex);
	ove_condvar_deinit(cv);
	ove_mutex_deinit(mutex);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	if (strcmp(argv[1], "timeout") == 0) {
		exercise("zero", 0, -EAGAIN, 0, OVE_ERR_TIMEOUT, false, false);
		exercise("positive", 2000000, -EAGAIN, 0, OVE_ERR_TIMEOUT, false, false);
		exercise("success-control", 2000000, 0, 0, OVE_OK, false, true);
	} else if (strcmp(argv[1], "success") == 0) {
		exercise("finite-success", 2000000, 0, 0, OVE_OK, false, true);
		exercise("forever-success", OVE_WAIT_FOREVER, 0, 0, OVE_OK, false, true);
	} else if (strcmp(argv[1], "error") == 0) {
		exercise("native-io", 2000000, -EIO, 0, OVE_ERR_IO, false, false);
		exercise("native-memory", 2000000, -ENOMEM, 0, OVE_ERR_NO_MEMORY, false, false);
		exercise("unexpected-timeout-code", 2000000, -ETIMEDOUT, 0, 0, true, false);
		exercise("success-control", 2000000, 0, 0, OVE_OK, false, true);
	} else if (strcmp(argv[1], "reacquire_error") == 0) {
		for (unsigned int i = 0; i < 3; ++i) {
			const int errors[] = {-EIO, -EAGAIN, -ETIMEDOUT};
			exercise("reacquire-error", 2000000, -EAGAIN, errors[i], 0, true, false);
		}
		exercise("success-control", 2000000, 0, 0, OVE_OK, false, true);
	} else if (strcmp(argv[1], "canary") == 0) {
		fixture_check(false, "explicit fixture failure canary");
		reset_model();
	} else {
		return 2;
	}
	return fixture_finish(argv[1]);
}
