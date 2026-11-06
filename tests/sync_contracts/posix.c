/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum kind { MUTEX, COND, SEM };
struct resource {
	void *address;
	enum kind kind;
	unsigned held;
};
static struct resource resources[8];
static unsigned failures, controls, native_inits, native_destroys, interventions;
static unsigned injected, wait_calls;
static const char *failure_site;
static pthread_mutexattr_t *active_attr;
static int attr_live, clobber_errno;
static int sem_error = EINVAL;
static int wait_status = EINVAL;
static struct ove_event *observed_event;

static void check(int condition, const char *message)
{
	if (!condition) {
		++failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

static void reject(void)
{
	fputs("fixture rejected: invalid observer operation\n", stderr);
	exit(2);
}

static int inject(const char *site)
{
	if (failure_site && strcmp(site, failure_site) == 0 && injected == 0) {
		++injected;
		return 1;
	}
	return 0;
}

static struct resource *lookup(void *address, enum kind kind)
{
	for (unsigned i = 0; i < 8; ++i)
		if (resources[i].address == address && resources[i].kind == kind)
			return &resources[i];
	return NULL;
}

static void add(void *address, enum kind kind)
{
	if (lookup(address, kind))
		reject();
	for (unsigned i = 0; i < 8; ++i) {
		if (!resources[i].address) {
			resources[i] = (struct resource){address, kind, 0};
			return;
		}
	}
	reject();
}

static unsigned live_count(void)
{
	unsigned count = 0;
	for (unsigned i = 0; i < 8; ++i)
		count += resources[i].address != NULL;
	return count;
}

static int record_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	++native_inits;
	if (lookup(mutex, MUTEX))
		reject();
	if (inject("mutex_init"))
		return ENOMEM;
	if (attr && (!attr_live || attr != active_attr)) {
		++interventions;
		check(0, "native mutex initialization requires live attributes");
		return EINVAL;
	}
	int rc = pthread_mutex_init(mutex, attr);
	if (!rc)
		add(mutex, MUTEX);
	return rc;
}

static int record_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	++native_inits;
	if (lookup(cond, COND))
		reject();
	if (inject("cond_init"))
		return ENOMEM;
	int rc = pthread_cond_init(cond, attr);
	if (!rc)
		add(cond, COND);
	return rc;
}

static int record_sem_init(sem_t *sem, int shared, unsigned value)
{
	++native_inits;
	if (lookup(sem, SEM))
		reject();
	if (value > SEM_VALUE_MAX) {
		++interventions;
		check(0, "native semaphore entry requires supported count");
		errno = EINVAL;
		return -1;
	}
	if (inject("sem_init")) {
		errno = ENOSPC;
		return -1;
	}
	int rc = sem_init(sem, shared, value);
	if (!rc)
		add(sem, SEM);
	return rc;
}

static int record_attr_init(pthread_mutexattr_t *attr)
{
	if (attr_live)
		reject();
	if (inject("attr_init"))
		return ENOMEM;
	int rc = pthread_mutexattr_init(attr);
	if (!rc) {
		active_attr = attr;
		attr_live = 1;
	}
	return rc;
}

static int record_attr_settype(pthread_mutexattr_t *attr, int type)
{
	if (!attr_live || attr != active_attr) {
		++interventions;
		check(0, "native attribute update requires initialized attributes");
		return EINVAL;
	}
	if (inject("attr_settype"))
		return EINVAL;
	return pthread_mutexattr_settype(attr, type);
}

static int record_attr_destroy(pthread_mutexattr_t *attr)
{
	if (!attr_live || attr != active_attr) {
		++interventions;
		check(0, "native attribute destruction requires initialized attributes");
		return EINVAL;
	}
	int rc = pthread_mutexattr_destroy(attr);
	if (rc)
		reject();
	active_attr = NULL;
	attr_live = 0;
	return rc;
}

static int record_mutex_destroy(pthread_mutex_t *mutex)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!r || r->held) {
		++interventions;
		check(0, "native mutex destruction requires an initialized unheld mutex");
		return EINVAL;
	}
	int rc = pthread_mutex_destroy(mutex);
	if (rc)
		reject();
	*r = (struct resource){0};
	++native_destroys;
	return rc;
}

static int record_cond_destroy(pthread_cond_t *cond)
{
	struct resource *r = lookup(cond, COND);
	if (!r) {
		++interventions;
		check(0, "native condition destruction requires initialization");
		return EINVAL;
	}
	int rc = pthread_cond_destroy(cond);
	if (rc)
		reject();
	*r = (struct resource){0};
	++native_destroys;
	return rc;
}

static int record_sem_destroy(sem_t *sem)
{
	struct resource *r = lookup(sem, SEM);
	if (!r) {
		++interventions;
		check(0, "native semaphore destruction requires initialization");
		return EINVAL;
	}
	int rc = sem_destroy(sem);
	if (rc)
		reject();
	*r = (struct resource){0};
	++native_destroys;
	return rc;
}

static int record_mutex_lock(pthread_mutex_t *mutex)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!r)
		reject();
	if (inject("lock"))
		return EAGAIN;
	int rc = pthread_mutex_lock(mutex);
	if (!rc)
		++r->held;
	return rc;
}

static int record_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *ts)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!r)
		reject();
	if (inject("lock"))
		return EAGAIN;
	int rc = pthread_mutex_timedlock(mutex, ts);
	if (!rc)
		++r->held;
	return rc;
}

static int record_mutex_unlock(pthread_mutex_t *mutex)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!r || !r->held) {
		++interventions;
		check(0, "native unlock requires proved ownership");
		return EPERM;
	}
	int rc = pthread_mutex_unlock(mutex);
	if (rc)
		reject();
	--r->held;
	return rc;
}

static int record_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!lookup(cond, COND) || !r || r->held != 1)
		reject();
	++wait_calls;
	if (inject("wait"))
		return wait_status;
	/* A bounded second return lets old Event's error loop finish safely.
	 * The caller holds this real mutex; no other thread accesses the latch. */
	if (observed_event && wait_calls == 2) {
		observed_event->signaled = 1;
		++interventions;
		return 0;
	}
	reject();
	return EINVAL;
}

static int record_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
				 const struct timespec *ts)
{
	struct resource *r = lookup(mutex, MUTEX);
	if (!lookup(cond, COND) || !r || r->held != 1)
		reject();
	++wait_calls;
	if (inject("wait"))
		return wait_status;
	if (observed_event && wait_calls == 2) {
		observed_event->signaled = 1;
		++interventions;
		return 0;
	}
	return pthread_cond_timedwait(cond, mutex, ts);
}

static int record_sem_wait(sem_t *sem)
{
	if (!lookup(sem, SEM))
		reject();
	if (inject("sem_wait")) {
		errno = sem_error;
		return -1;
	}
	return sem_wait(sem);
}

static int record_sem_timedwait(sem_t *sem, const struct timespec *ts)
{
	if (!lookup(sem, SEM))
		reject();
	if (inject("sem_wait")) {
		errno = sem_error;
		return -1;
	}
	return sem_timedwait(sem, ts);
}

void ove_backend_thread_set_state(int state)
{
	(void)state;
	if (clobber_errno)
		errno = ETIMEDOUT;
}

#define pthread_mutex_init record_mutex_init
#define pthread_cond_init record_cond_init
#define sem_init record_sem_init
#define pthread_mutexattr_init record_attr_init
#define pthread_mutexattr_settype record_attr_settype
#define pthread_mutexattr_destroy record_attr_destroy
#define pthread_mutex_destroy record_mutex_destroy
#define pthread_cond_destroy record_cond_destroy
#define sem_destroy record_sem_destroy
#define pthread_mutex_lock record_mutex_lock
#define pthread_mutex_timedlock record_mutex_timedlock
#define pthread_mutex_unlock record_mutex_unlock
#define pthread_cond_wait record_cond_wait
#define pthread_cond_timedwait record_cond_timedwait
#define sem_wait record_sem_wait
#define sem_timedwait record_sem_timedwait
#include OVE_POSIX_SYNC_SOURCE
#undef pthread_mutex_init
#undef pthread_cond_init
#undef sem_init
#undef pthread_mutexattr_init
#undef pthread_mutexattr_settype
#undef pthread_mutexattr_destroy
#undef pthread_mutex_destroy
#undef pthread_cond_destroy
#undef sem_destroy
#undef pthread_mutex_lock
#undef pthread_mutex_timedlock
#undef pthread_mutex_unlock
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef sem_wait
#undef sem_timedwait

enum family { NORMAL, RECURSIVE, SEMAPHORE, EVENT, CONDVAR };
union backing {
	ove_mutex_storage_t mutex;
	ove_sem_storage_t sem;
	ove_event_storage_t event;
	ove_condvar_storage_t cv;
};
union handle {
	ove_mutex_t mutex;
	ove_sem_t sem;
	ove_event_t event;
	ove_condvar_t cv;
};

static int initialize(enum family family, union backing *backing, union handle *out,
		      unsigned initial, unsigned max)
{
	switch (family) {
	case NORMAL:
		return ove_mutex_init(&out->mutex, &backing->mutex);
	case RECURSIVE:
		return ove_recursive_mutex_init(&out->mutex, &backing->mutex);
	case SEMAPHORE:
		return ove_sem_init(&out->sem, &backing->sem, initial, max);
	case EVENT:
		return ove_event_init(&out->event, &backing->event);
	case CONDVAR:
		return ove_condvar_init(&out->cv, &backing->cv);
	}
	reject();
	return OVE_ERR_INVALID_PARAM;
}

static union handle sentinel_handle(enum family family, union backing *sentinel)
{
	union handle out = {0};
	switch (family) {
	case NORMAL:
	case RECURSIVE:
		out.mutex = &sentinel->mutex;
		break;
	case SEMAPHORE:
		out.sem = &sentinel->sem;
		break;
	case EVENT:
		out.event = &sentinel->event;
		break;
	case CONDVAR:
		out.cv = &sentinel->cv;
		break;
	}
	return out;
}

static int same_handle(enum family family, union handle a, union handle b)
{
	switch (family) {
	case NORMAL:
	case RECURSIVE:
		return a.mutex == b.mutex;
	case SEMAPHORE:
		return a.sem == b.sem;
	case EVENT:
		return a.event == b.event;
	case CONDVAR:
		return a.cv == b.cv;
	}
	return 0;
}

static void private_cleanup(void)
{
	/* Every address still designates initialized backing in the caller. */
	for (unsigned i = 0; i < 8; ++i) {
		struct resource *r = &resources[i];
		if (!r->address)
			continue;
		while (r->kind == MUTEX && r->held)
			if (record_mutex_unlock(r->address))
				reject();
		int rc = r->kind == MUTEX  ? record_mutex_destroy(r->address)
			 : r->kind == COND ? record_cond_destroy(r->address)
					   : record_sem_destroy(r->address);
		if (rc)
			reject();
	}
}

static void ordinary_controls(void)
{
	if (failure_site || live_count() || attr_live)
		reject();
	for (unsigned cycle = 0; cycle < 2; ++cycle) {
		ove_mutex_storage_t mutex = {0}, recursive = {0};
		ove_sem_storage_t sem = {0};
		ove_event_storage_t event = {0};
		ove_condvar_storage_t cv = {0};
		ove_mutex_t m = NULL, rm = NULL;
		ove_sem_t s = NULL;
		ove_event_t e = NULL;
		ove_condvar_t c = NULL;
		if (ove_mutex_init(&m, &mutex) || ove_recursive_mutex_init(&rm, &recursive) ||
		    ove_sem_init(&s, &sem, 1, 2) || ove_event_init(&e, &event) ||
		    ove_condvar_init(&c, &cv))
			reject();
		if (ove_mutex_lock(m, OVE_WAIT_FOREVER) ||
		    ove_recursive_mutex_lock(rm, OVE_WAIT_FOREVER) ||
		    ove_recursive_mutex_lock(rm, 0) || ove_sem_take(s, 0))
			reject();
		check(ove_condvar_wait(c, m, 0) == OVE_ERR_TIMEOUT,
		      "actual condition timeout preserves the mutex");
		ove_mutex_unlock(m);
		ove_recursive_mutex_unlock(rm);
		ove_recursive_mutex_unlock(rm);
		ove_sem_give(s);
		if (ove_sem_take(s, OVE_WAIT_FOREVER))
			reject();
		ove_event_signal(e);
		if (ove_event_wait(e, OVE_WAIT_FOREVER))
			reject();
		check(ove_event_wait(e, 0) == OVE_ERR_TIMEOUT, "actual event clean timeout");
		ove_event_signal_from_isr(e);
		check(ove_event_wait(e, 0) == OVE_OK,
		      "thread caller delegates the ISR signal variant");
		ove_condvar_deinit(c);
		ove_event_deinit(e);
		ove_sem_deinit(s);
		ove_recursive_mutex_deinit(rm);
		ove_mutex_deinit(m);
		if (live_count() || attr_live)
			reject();
		++controls;
	}
	ove_sem_storage_t boundary = {0};
	ove_sem_t full = NULL;
	int rc = ove_sem_init(&full, &boundary, SEM_VALUE_MAX, SEM_VALUE_MAX);
	check(rc == OVE_OK, "provider maximum count remains accepted");
	if (rc == OVE_OK) {
		if (ove_sem_take(full, 0))
			reject();
		ove_sem_deinit(full);
	}
	full = NULL;
	check(live_count() == 0, "maximum-count control releases native resources");
	private_cleanup();
	ove_sem_storage_t empty = {0};
	ove_sem_t zero = NULL;
	if (ove_sem_init(&zero, &empty, 0, 2))
		reject();
	ove_sem_give(zero);
	if (ove_sem_take(zero, 0))
		reject();
	ove_sem_deinit(zero);
	wait_calls = 0;
}

static void reuse_backing(enum family family, union backing *backing)
{
	if (live_count() || attr_live || failure_site)
		reject();
	union handle out = {0};
	if (initialize(family, backing, &out, 1, 2))
		reject();
	switch (family) {
	case NORMAL:
		if (ove_mutex_lock(out.mutex, 0))
			reject();
		ove_mutex_unlock(out.mutex);
		ove_mutex_deinit(out.mutex);
		break;
	case RECURSIVE:
		if (ove_recursive_mutex_lock(out.mutex, 0) ||
		    ove_recursive_mutex_lock(out.mutex, 0))
			reject();
		ove_recursive_mutex_unlock(out.mutex);
		ove_recursive_mutex_unlock(out.mutex);
		ove_recursive_mutex_deinit(out.mutex);
		break;
	case SEMAPHORE:
		if (ove_sem_take(out.sem, 0))
			reject();
		ove_sem_give(out.sem);
		if (ove_sem_take(out.sem, OVE_WAIT_FOREVER))
			reject();
		ove_sem_deinit(out.sem);
		break;
	case EVENT:
		ove_event_signal(out.event);
		if (ove_event_wait(out.event, 0))
			reject();
		ove_event_deinit(out.event);
		break;
	case CONDVAR: {
		ove_mutex_storage_t mutex = {0};
		ove_mutex_t m = NULL;
		if (ove_mutex_init(&m, &mutex) || ove_mutex_lock(m, 0))
			reject();
		check(ove_condvar_wait(out.cv, m, 0) == OVE_ERR_TIMEOUT,
		      "reused condition variable has a clean native timeout");
		ove_mutex_unlock(m);
		ove_mutex_deinit(m);
		ove_condvar_deinit(out.cv);
		break;
	}
	}
	check(live_count() == 0 && !attr_live, "same backing retry releases all native pieces");
	++controls;
}

static void constructor_case(enum family family, const char *site, int expected)
{
	union backing backing = {0}, sentinel = {0};
	union handle original = sentinel_handle(family, &sentinel), out = original;
	failure_site = site;
	int rc = initialize(family, &backing, &out, 0, 1);
	check(injected == 1, "constructor error injection reached once");
	check(rc == expected, "constructor preserves native failure");
	check(same_handle(family, out, original), "failed constructor preserves output");
	check(live_count() == 0, "failed constructor releases initialized pieces");
	check(!attr_live, "recursive attributes are released");
	/* Never deinitialize a handle merely because old code published it. */
	out = (union handle){0};
	private_cleanup();
	failure_site = NULL;
	reuse_backing(family, &backing);
	reuse_backing(family, &backing);
}

static void count_case(unsigned initial, unsigned max)
{
	ove_sem_storage_t storage = {0}, before, sentinel = {0};
	memcpy(&before, &storage, sizeof(storage));
	ove_sem_t out = &sentinel;
	unsigned entries = native_inits;
	int rc = ove_sem_init(&out, &storage, initial, max);
	check(rc == OVE_ERR_INVALID_PARAM, "invalid count is rejected");
	check(out == &sentinel, "invalid count preserves output");
	check(native_inits == entries, "invalid count never enters provider");
	check(memcmp(&before, &storage, sizeof(storage)) == 0,
	      "invalid count leaves backing unchanged");
	out = NULL;
	private_cleanup();
	ove_sem_t retry_handle = NULL;
	if (ove_sem_init(&retry_handle, &storage, 1, 2) || ove_sem_take(retry_handle, 0))
		reject();
	ove_sem_deinit(retry_handle);
	if (ove_sem_init(&retry_handle, &storage, 1, 2) || ove_sem_take(retry_handle, 0))
		reject();
	ove_sem_deinit(retry_handle);
	check(live_count() == 0, "rejected semaphore backing can be initialized and released");
	++controls;
}

enum status_family { MUTEX_ERROR, CONDVAR_ERROR, EVENT_ERROR, SEM_ERROR };

static void status_case(enum status_family family, int forever, int injected_status)
{
	ove_mutex_storage_t mutex = {0};
	ove_condvar_storage_t cv = {0};
	ove_sem_storage_t sem = {0};
	ove_event_storage_t event = {0};
	ove_mutex_t m = NULL;
	ove_condvar_t c = NULL;
	ove_sem_t s = NULL;
	ove_event_t e = NULL;
	uint64_t timeout = forever ? OVE_WAIT_FOREVER : 0;
	wait_status = injected_status;
	int expected = injected_status == EINVAL ? OVE_ERR_INVALID_PARAM : OVE_ERR_BUSY;
	int rc;
	if (family == MUTEX_ERROR) {
		if (ove_recursive_mutex_init(&m, &mutex))
			reject();
		failure_site = "lock";
		rc = ove_recursive_mutex_lock(m, timeout);
		check(rc == OVE_ERR_BUSY, "recursive exhaustion is not acquisition or timeout");
	} else if (family == CONDVAR_ERROR) {
		if (ove_mutex_init(&m, &mutex) || ove_condvar_init(&c, &cv) ||
		    ove_mutex_lock(m, OVE_WAIT_FOREVER))
			reject();
		failure_site = "wait";
		rc = ove_condvar_wait(c, m, timeout);
		check(rc == expected, "condition wait preserves non-clean error");
	} else if (family == EVENT_ERROR) {
		if (ove_event_init(&e, &event))
			reject();
		observed_event = e;
		failure_site = "wait";
		rc = ove_event_wait(e, timeout);
		check(rc == expected, "event wait preserves defined native error");
		check(wait_calls == 1, "event error cannot resume its wait loop");
		check(lookup(&event.lock, MUTEX)->held == 0,
		      "defined default-mutex error releases the event lock");
		observed_event = NULL;
	} else {
		if (ove_sem_init(&s, &sem, 0, 1))
			reject();
		failure_site = "sem_wait";
		sem_error = forever ? ENOSPC : EINVAL;
		clobber_errno = 1;
		rc = ove_sem_take(s, timeout);
		check(rc == (forever ? OVE_ERR_NO_SPACE : OVE_ERR_INVALID_PARAM),
		      "semaphore preserves error before state hooks");
		clobber_errno = 0;
	}
	check(injected == 1, "status error injection reached once");
	failure_site = NULL;
	private_cleanup();
}

static void event_lock_case(int signal_failure)
{
	ove_event_storage_t storage = {0};
	ove_event_t event = NULL;
	if (ove_event_init(&event, &storage))
		reject();
	if (!signal_failure)
		ove_event_signal(event);
	failure_site = "lock";
	if (signal_failure) {
		ove_event_signal(event);
	} else {
		check(ove_event_wait(event, 0) == OVE_ERR_BUSY,
		      "event initial acquisition error is preserved");
	}
	check(injected == 1, "event lock injection reached once");
	check(lookup(&storage.lock, MUTEX)->held == 0, "failed event lock is not unlocked");
	failure_site = NULL;
	check(ove_event_wait(event, 0) == (signal_failure ? OVE_ERR_TIMEOUT : OVE_OK),
	      "event lock error preserves the existing latch");
	ove_event_deinit(event);
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
				      "count_zero",
				      "count_order",
				      "count_initial_domain",
				      "count_max_domain",
				      "mutex_forever_error",
				      "mutex_timed_error",
				      "condvar_forever_error",
				      "condvar_timed_error",
				      "event_forever_error",
				      "event_timed_error",
				      "sem_forever_error",
				      "sem_timed_error",
				      "count_initial_ceiling_plus_one",
				      "count_max_ceiling_plus_one",
				      "event_wait_lock_error",
				      "event_signal_lock_error",
				      "condvar_forever_eagain_error",
				      "condvar_timed_eagain_error",
				      "event_forever_eagain_error",
				      "event_timed_eagain_error",
				      "condvar_forever_timeout_error",
				      "event_forever_timeout_error",
				      "controls",
				      "canary"};
	int known = 0;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		known |= strcmp(name, names[i]) == 0;
	if (!known)
		return 2;
	if (strcmp(name, "canary") == 0)
		check(0, "explicit early failure canary");
	ordinary_controls();
	if (strcmp(name, "mutex_init") == 0)
		constructor_case(NORMAL, "mutex_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "recursive_attr_init") == 0)
		constructor_case(RECURSIVE, "attr_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "recursive_attr_type") == 0)
		constructor_case(RECURSIVE, "attr_settype", OVE_ERR_INVALID_PARAM);
	else if (strcmp(name, "recursive_mutex_init") == 0)
		constructor_case(RECURSIVE, "mutex_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "sem_init") == 0)
		constructor_case(SEMAPHORE, "sem_init", OVE_ERR_NO_SPACE);
	else if (strcmp(name, "event_mutex_init") == 0)
		constructor_case(EVENT, "mutex_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "event_cond_init") == 0)
		constructor_case(EVENT, "cond_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "condvar_init") == 0)
		constructor_case(CONDVAR, "cond_init", OVE_ERR_NO_MEMORY);
	else if (strcmp(name, "count_zero") == 0)
		count_case(0, 0);
	else if (strcmp(name, "count_order") == 0)
		count_case(2, 1);
	else if (strcmp(name, "count_initial_domain") == 0)
		count_case(UINT_MAX, UINT_MAX);
	else if (strcmp(name, "count_max_domain") == 0)
		count_case(0, UINT_MAX);
	else if (strcmp(name, "count_initial_ceiling_plus_one") == 0)
		count_case((unsigned)SEM_VALUE_MAX + 1u, (unsigned)SEM_VALUE_MAX + 1u);
	else if (strcmp(name, "count_max_ceiling_plus_one") == 0)
		count_case(0, (unsigned)SEM_VALUE_MAX + 1u);
	else if (strcmp(name, "event_wait_lock_error") == 0)
		event_lock_case(0);
	else if (strcmp(name, "event_signal_lock_error") == 0)
		event_lock_case(1);
	else if (strcmp(name, "mutex_forever_error") == 0)
		status_case(MUTEX_ERROR, 1, EINVAL);
	else if (strcmp(name, "mutex_timed_error") == 0)
		status_case(MUTEX_ERROR, 0, EINVAL);
	else if (strcmp(name, "condvar_forever_error") == 0)
		status_case(CONDVAR_ERROR, 1, EINVAL);
	else if (strcmp(name, "condvar_timed_error") == 0)
		status_case(CONDVAR_ERROR, 0, EINVAL);
	else if (strcmp(name, "event_forever_error") == 0)
		status_case(EVENT_ERROR, 1, EINVAL);
	else if (strcmp(name, "event_timed_error") == 0)
		status_case(EVENT_ERROR, 0, EINVAL);
	else if (strcmp(name, "sem_forever_error") == 0)
		status_case(SEM_ERROR, 1, EINVAL);
	else if (strcmp(name, "sem_timed_error") == 0)
		status_case(SEM_ERROR, 0, EINVAL);
	else if (strcmp(name, "condvar_forever_eagain_error") == 0)
		status_case(CONDVAR_ERROR, 1, EAGAIN);
	else if (strcmp(name, "condvar_timed_eagain_error") == 0)
		status_case(CONDVAR_ERROR, 0, EAGAIN);
	else if (strcmp(name, "event_forever_eagain_error") == 0)
		status_case(EVENT_ERROR, 1, EAGAIN);
	else if (strcmp(name, "event_timed_eagain_error") == 0)
		status_case(EVENT_ERROR, 0, EAGAIN);
	else if (strcmp(name, "condvar_forever_timeout_error") == 0)
		status_case(CONDVAR_ERROR, 1, ETIMEDOUT);
	else if (strcmp(name, "event_forever_timeout_error") == 0)
		status_case(EVENT_ERROR, 1, ETIMEDOUT);
	else if (strcmp(name, "controls") && strcmp(name, "canary"))
		return 2;
	check(live_count() == 0 && !attr_live, "all real resources released before backing ends");
	printf("%s: failures=%u controls=%u injected=%u native_inits=%u destroys=%u safety=%u\n",
	       name, failures, controls, injected, native_inits, native_destroys, interventions);
	return failures ? 1 : 0;
}
