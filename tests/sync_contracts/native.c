/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Complete SDK models record adapter dispatch. They do not execute kernels or
 * establish target ABI, allocator exhaustion, scheduling or IRQ behavior. */
static unsigned failures, controls, init_calls, lock_calls, fail_init, fail_lock;
static unsigned safety, private_repairs;
static int wait_error;
static int lock_error = -EIO;

static void check(int condition, const char *message)
{
	if (!condition) {
		++failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

static void reject(void)
{
	fputs("fixture rejected: invalid SDK observer operation\n", stderr);
	exit(2);
}

struct record {
	void *address;
	unsigned kind, held, count, maximum;
#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OBJ_CORE)
	struct k_obj_core *core;
#endif
};
static struct record records[10];

static struct record *find(void *address)
{
	if (!address)
		return NULL;
	for (unsigned i = 0; i < 10; ++i)
		if (records[i].address == address)
			return &records[i];
	return NULL;
}

static unsigned live_count(void)
{
	unsigned count = 0;
	for (unsigned i = 0; i < 10; ++i)
		count += records[i].address != NULL;
	return count;
}

static int begin_init(void *address, unsigned kind, unsigned count, unsigned maximum)
{
	if (find(address))
		reject();
	++init_calls;
	if (fail_init == init_calls)
		return -EIO;
	for (unsigned i = 0; i < 10; ++i) {
		if (!records[i].address) {
			records[i] = (struct record){.address = address,
						     .kind = kind,
						     .count = count,
						     .maximum = maximum};
			return 0;
		}
	}
	reject();
	return -EIO;
}

static void retire(void *address)
{
	struct record *r = find(address);
	if (!r || r->held) {
		++safety;
		check(0, "native retirement requires an initialized unheld primitive");
		return;
	}
	*r = (struct record){0};
}

static int acquire(void *address)
{
	struct record *r = find(address);
	if (!r)
		reject();
	++lock_calls;
	if (fail_lock == lock_calls)
		return lock_error;
	++r->held;
	return 0;
}

static int release(void *address)
{
	struct record *r = find(address);
	if (!r || !r->held) {
		++safety;
		check(0, "native unlock requires proved acquisition");
		return -EPERM;
	}
	--r->held;
	return 0;
}

static int take(void *address)
{
	struct record *r = find(address);
	if (!r)
		reject();
	if (wait_error)
		return wait_error;
	if (r->count) {
		--r->count;
		return 0;
	}
	return -ETIMEDOUT;
}

static void give(void *address)
{
	struct record *r = find(address);
	if (!r)
		reject();
	if (r->count < r->maximum)
		++r->count;
}

#ifdef CONFIG_OVE_RTOS_FREERTOS
struct model_task {
	unsigned initialized;
};
static struct model_task task = {1};
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
UBaseType_t model_enter_critical_from_isr(void)
{
	UBaseType_t saved = critical_depth;
	model_enter_critical();
	return saved;
}
void model_exit_critical_from_isr(UBaseType_t saved)
{
	if (critical_depth != saved + 1)
		reject();
	model_exit_critical();
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &task;
}
TickType_t xTaskGetTickCount(void)
{
	return 1;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	(void)clear;
	(void)ticks;
	return 0;
}
BaseType_t xTaskNotifyGive(TaskHandle_t target)
{
	(void)target;
	reject();
	return pdFALSE;
}
void vTaskNotifyGiveFromISR(TaskHandle_t target, BaseType_t *wake)
{
	(void)target;
	(void)wake;
	reject();
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
	if (begin_init(storage, 1, 0, 0))
		return NULL;
	storage->initialized = true;
	return storage;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *storage)
{
	return xSemaphoreCreateMutexStatic(storage);
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned maximum, unsigned initial,
						 StaticSemaphore_t *storage)
{
	if (!maximum || initial > maximum) {
		++init_calls;
		++safety;
		check(0, "count validation precedes the native assertion boundary");
		return NULL;
	}
	if (begin_init(storage, 2, initial, maximum))
		return NULL;
	storage->initialized = true;
	return storage;
}
void vSemaphoreDelete(SemaphoreHandle_t sem)
{
	retire(sem);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
	(void)ticks;
	struct record *r = find(sem);
	if (!r)
		reject();
	return (r->kind == 1 ? acquire(sem) : take(sem)) == 0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
	struct record *r = find(sem);
	if (!r)
		reject();
	if (r->kind == 1)
		return release(sem) == 0 ? pdTRUE : pdFALSE;
	give(sem);
	return pdTRUE;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks)
{
	return xSemaphoreTake(sem, ticks);
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem)
{
	return xSemaphoreGive(sem);
}
#define COUNT_MAX UINT_MAX
#define INIT_FAILURE OVE_ERR_NO_MEMORY
#endif

#ifdef CONFIG_OVE_RTOS_ZEPHYR
static struct k_thread current_thread;
#ifdef CONFIG_OBJ_CORE
static struct k_obj_core *core_head;
static void link_core(void *address, struct k_obj_core *core)
{
	struct record *r = find(address);
	if (!r || core->linked)
		reject();
	core->next = core_head;
	core->linked = true;
	core_head = core;
	r->core = core;
}
void k_obj_core_unlink(struct k_obj_core *core)
{
	struct record *record = NULL;
	for (unsigned i = 0; i < 10; ++i)
		if (records[i].address && records[i].core == core)
			record = &records[i];
	if (!record || record->held) {
		++safety;
		check(0, "object-core unlink requires a live unheld primitive");
		return;
	}
	struct k_obj_core **link = &core_head;
	while (*link && *link != core)
		link = &(*link)->next;
	if (!*link) {
		++safety;
		check(0, "object-core unlink requires a live linked node");
		return;
	}
	*link = core->next;
	core->next = NULL;
	core->linked = false;
	retire(record->address);
}
#endif
int k_mutex_init(struct k_mutex *mutex)
{
	int rc = begin_init(mutex, 1, 0, 0);
	if (rc)
		return rc;
	mutex->initialized = true;
	mutex->owner = NULL;
	mutex->lock_count = 0;
#ifdef CONFIG_OBJ_CORE_MUTEX
	link_core(mutex, &mutex->obj_core);
#endif
	return 0;
}
int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)timeout;
	int rc = acquire(mutex);
	if (!rc) {
		mutex->owner = &current_thread;
		++mutex->lock_count;
	}
	return rc;
}
int k_mutex_unlock(struct k_mutex *mutex)
{
	int rc = release(mutex);
	if (!rc && --mutex->lock_count == 0)
		mutex->owner = NULL;
	return rc;
}
int k_sem_init(struct k_sem *sem, unsigned initial, unsigned maximum)
{
	if (!maximum || initial > maximum) {
		++init_calls;
		++safety;
		check(0, "count validation precedes native semaphore entry");
		return -EINVAL;
	}
	int rc = begin_init(sem, 2, initial, maximum);
	if (rc)
		return rc;
	sem->initialized = true;
	sem->count = initial;
	sem->limit = maximum;
#ifdef CONFIG_OBJ_CORE_SEM
	link_core(sem, &sem->obj_core);
#endif
	return 0;
}
int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	int rc = take(sem);
	return rc == -ETIMEDOUT ? (timeout.ticks == 0 ? -EBUSY : -EAGAIN) : rc;
}
void k_sem_give(struct k_sem *sem)
{
	give(sem);
}
int k_condvar_init(struct k_condvar *cv)
{
	int rc = begin_init(cv, 3, 0, 0);
	if (rc)
		return rc;
	cv->initialized = true;
#ifdef CONFIG_OBJ_CORE_CONDVAR
	link_core(cv, &cv->obj_core);
#endif
	return 0;
}
int k_condvar_signal(struct k_condvar *cv)
{
	if (!find(cv))
		reject();
	return 0;
}
int k_condvar_broadcast(struct k_condvar *cv)
{
	return k_condvar_signal(cv);
}
int k_condvar_wait(struct k_condvar *cv, struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)cv;
	(void)mutex;
	(void)timeout;
	reject();
	return -EIO;
}
#define COUNT_MAX UINT_MAX
#define INIT_FAILURE OVE_ERR_IO
#endif

#ifdef CONFIG_OVE_RTOS_NUTTX
#include <nuttx/semaphore.h>

enum cv_role { CV_ENTRY = 1, CV_CALLER, CV_RETIRE };
static struct {
	ove_condvar_t cv;
	mutex_t *caller;
	unsigned failure_role, remaining, injections, posts;
	int error;
	bool active, waited, scheduler, signal_token;
	char trace[256];
} cv_observer;

static void cv_note(char role)
{
	if (!cv_observer.active || cv_observer.scheduler)
		return;
	struct record *caller = find(cv_observer.caller);
	struct record *guard = find(&cv_observer.cv->guard);
	size_t used = strlen(cv_observer.trace);
	int count = snprintf(cv_observer.trace + used, sizeof(cv_observer.trace) - used,
			     "%s%c%d:%d:%d", used ? "/" : "", role, cv_observer.cv->nwaiters,
			     caller ? (int)caller->held : -1, guard ? (int)guard->held : -1);
	if (count < 0 || (size_t)count >= sizeof(cv_observer.trace) - used)
		reject();
}

static void cv_lock_attempt(mutex_t *mutex)
{
	if (!cv_observer.active || cv_observer.scheduler)
		return;
	unsigned role;
	if (mutex == cv_observer.caller) {
		role = CV_CALLER;
		cv_note('M');
	} else if (mutex == &cv_observer.cv->guard) {
		role = cv_observer.waited ? CV_RETIRE : CV_ENTRY;
		cv_note(role == CV_ENTRY ? 'G' : 'R');
	} else {
		reject();
		return;
	}
	if (role == cv_observer.failure_role && cv_observer.remaining) {
		--cv_observer.remaining;
		++cv_observer.injections;
		fail_lock = lock_calls + 1;
		lock_error = cv_observer.error;
	}
}

int nxmutex_init(mutex_t *mutex)
{
	int rc = begin_init(mutex, 1, 0, 0);
	if (!rc) {
		mutex->initialized = true;
		mutex->held = 0;
	}
	return rc;
}
int nxmutex_destroy(mutex_t *mutex)
{
	retire(mutex);
	return 0;
}
int nxmutex_lock(mutex_t *mutex)
{
	cv_lock_attempt(mutex);
	int rc = acquire(mutex);
	if (!rc)
		++mutex->held;
	return rc;
}
int nxmutex_ticklock(mutex_t *mutex, uint32_t ticks)
{
	(void)ticks;
	return nxmutex_lock(mutex);
}
int nxmutex_unlock(mutex_t *mutex)
{
	if (cv_observer.active && !cv_observer.scheduler) {
		if (mutex == cv_observer.caller)
			cv_note('m');
		else if (mutex == &cv_observer.cv->guard)
			cv_note(cv_observer.waited ? 'r' : 'g');
		else
			reject();
	}
	int rc = release(mutex);
	if (!rc)
		--mutex->held;
	return rc;
}
int nxrmutex_init(rmutex_t *mutex)
{
	mutex->count = 0;
	return nxmutex_init(&mutex->mutex);
}
int nxrmutex_destroy(rmutex_t *mutex)
{
	return nxmutex_destroy(&mutex->mutex);
}
int nxrmutex_lock(rmutex_t *mutex)
{
	return nxmutex_lock(&mutex->mutex);
}
int nxrmutex_ticklock(rmutex_t *mutex, uint32_t ticks)
{
	return nxmutex_ticklock(&mutex->mutex, ticks);
}
int nxrmutex_unlock(rmutex_t *mutex)
{
	return nxmutex_unlock(&mutex->mutex);
}
int nxsem_init(sem_t *sem, int shared, int32_t count)
{
	(void)shared;
	if (count < 0) {
		++init_calls;
		++safety;
		check(0, "count validation precedes signed native conversion");
		return -EINVAL;
	}
	int rc = begin_init(sem, 2, (unsigned)count, INT_MAX);
	if (!rc) {
		sem->initialized = true;
		sem->count = count;
	}
	return rc;
}
int nxsem_destroy(sem_t *sem)
{
	retire(sem);
	return 0;
}

static int cv_take(sem_t *sem, bool poll)
{
	if (cv_observer.active && sem == &cv_observer.cv->waiter) {
		cv_note('W');
		cv_observer.waited = true;
		if (wait_error)
			++cv_observer.injections;
		if (cv_observer.signal_token) {
			/* A sequential signal step posts an actual initialized model
			 * token while holding the bookkeeping guard. */
			cv_observer.scheduler = true;
			if (nxmutex_lock(&cv_observer.cv->guard) || nxsem_post(sem) ||
			    nxmutex_unlock(&cv_observer.cv->guard))
				reject();
			cv_observer.scheduler = false;
		}
	}
	int rc = take(sem);
	return rc == -ETIMEDOUT && poll ? -EAGAIN : rc;
}

int nxsem_wait_uninterruptible(sem_t *sem)
{
	return cv_take(sem, false);
}
int nxsem_tickwait_uninterruptible(sem_t *sem, uint32_t ticks)
{
	return cv_take(sem, ticks == 0);
}
int nxsem_post(sem_t *sem)
{
	if (cv_observer.active && sem == &cv_observer.cv->waiter) {
		cv_note('P');
		struct record *guard = find(&cv_observer.cv->guard);
		if (!guard || guard->held != 1) {
			++safety;
			check(0, "condition post requires acquired bookkeeping guard");
			return -EPERM;
		}
		++cv_observer.posts;
	}
	give(sem);
	return 0;
}
int nxsem_get_value(sem_t *sem, int *value)
{
	struct record *r = find(sem);
	if (!r)
		reject();
	*value = (int)r->count;
	return 0;
}
unsigned enter_critical_section(void)
{
	return 0;
}
void leave_critical_section(unsigned flags)
{
	(void)flags;
}
#define COUNT_MAX INT_MAX
#define INIT_FAILURE OVE_ERR_IO
#endif

#include OVE_SYNC_BACKEND_SOURCE

static void private_cleanup(void)
{
	for (unsigned i = 0; i < 10; ++i) {
		struct record *r = &records[i];
		if (!r->address)
			continue;
		while (r->held) {
#ifdef CONFIG_OVE_RTOS_FREERTOS
			if (xSemaphoreGive(r->address) != pdTRUE)
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
			if (k_mutex_unlock(r->address))
#else
			if (nxmutex_unlock(r->address))
#endif
				reject();
		}
#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OBJ_CORE)
		if (!r->core || !r->core->linked)
			reject();
		k_obj_core_unlink(r->core);
#else
		retire(r->address);
#endif
		++private_repairs;
	}
}

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

static union handle sentinel_handle(enum family family, union backing *storage)
{
	union handle out = {0};
	if (family <= RECURSIVE)
		out.mutex = &storage->mutex;
	else if (family == SEMAPHORE)
		out.sem = &storage->sem;
	else if (family == EVENT)
		out.event = &storage->event;
	else
		out.cv = &storage->cv;
	return out;
}

static int same_handle(enum family family, union handle a, union handle b)
{
	if (family <= RECURSIVE)
		return a.mutex == b.mutex;
	if (family == SEMAPHORE)
		return a.sem == b.sem;
	if (family == EVENT)
		return a.event == b.event;
	return a.cv == b.cv;
}

static int initialize(enum family family, union backing *storage, union handle *out,
		      unsigned initial, unsigned maximum)
{
	if (family == NORMAL)
		return ove_mutex_init(&out->mutex, &storage->mutex);
	if (family == RECURSIVE)
		return ove_recursive_mutex_init(&out->mutex, &storage->mutex);
	if (family == SEMAPHORE)
		return ove_sem_init(&out->sem, &storage->sem, initial, maximum);
	if (family == EVENT)
		return ove_event_init(&out->event, &storage->event);
	return ove_condvar_init(&out->cv, &storage->cv);
}

static void deinitialize(enum family family, union handle out)
{
	if (family == NORMAL)
		ove_mutex_deinit(out.mutex);
	else if (family == RECURSIVE)
		ove_recursive_mutex_deinit(out.mutex);
	else if (family == SEMAPHORE)
		ove_sem_deinit(out.sem);
	else if (family == EVENT)
		ove_event_deinit(out.event);
	else
		ove_condvar_deinit(out.cv);
}

#ifndef CONFIG_OVE_RTOS_FREERTOS
static void wait_class_controls(enum family family, union handle out)
{
	for (unsigned finite = 0; finite < 2; ++finite) {
#ifdef CONFIG_OVE_RTOS_ZEPHYR
		int status = finite ? -EAGAIN : -EBUSY;
#else
		int status = finite ? -ETIMEDOUT : -EAGAIN;
#endif
		int rc;
		if (family <= RECURSIVE) {
			/* Native return-class dispatch only; no scheduler is modeled. */
			fail_lock = lock_calls + 1;
			lock_error = status;
			rc = family == NORMAL ? ove_mutex_lock(out.mutex, finite)
					      : ove_recursive_mutex_lock(out.mutex, finite);
		} else {
			wait_error = status;
			rc = family == SEMAPHORE ? ove_sem_take(out.sem, finite)
						 : ove_event_wait(out.event, finite);
		}
		check(rc == OVE_ERR_TIMEOUT, "native poll and expiry classes remain timeouts");
		fail_lock = 0;
		wait_error = 0;
		lock_error = -EIO;
	}
}
#endif

static void reuse_backing(enum family family, union backing *storage)
{
	union handle out = {0};
	if (live_count() || fail_init || fail_lock || wait_error)
		reject();
	if (initialize(family, storage, &out, 1, 2))
		reject();
	if (family <= RECURSIVE) {
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
		ove_sem_give(out.sem);
		if (ove_sem_take(out.sem, 0))
			reject();
	} else if (family == EVENT) {
		ove_event_signal(out.event);
		if (ove_event_wait(out.event, 0))
			reject();
	} else {
		ove_condvar_signal(out.cv);
	}
#ifndef CONFIG_OVE_RTOS_FREERTOS
	if (family <= EVENT)
		wait_class_controls(family, out);
#endif
	deinitialize(family, out);
#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OBJ_CORE)
	check(live_count() == 0 && core_head == NULL,
	      "quiescent deinit unlinks all object-core nodes");
#elif !defined(CONFIG_OVE_RTOS_ZEPHYR)
	check(live_count() == 0, "quiescent deinit releases native resources");
#endif
	out = (union handle){0};
	/* Observe the parent defect before privately retiring still-live metadata. */
	private_cleanup();
	++controls;
}

static void ordinary_controls(void)
{
	for (enum family family = NORMAL; family <= CONDVAR; ++family) {
		union backing storage = {0};
		reuse_backing(family, &storage);
		reuse_backing(family, &storage);
	}
	ove_sem_storage_t storage = {0};
	ove_sem_t sem = NULL;
	int rc = ove_sem_init(&sem, &storage, COUNT_MAX, COUNT_MAX);
	check(rc == OVE_OK, "provider maximum count remains accepted");
	if (rc == OVE_OK) {
		if (ove_sem_take(sem, 0))
			reject();
		ove_sem_deinit(sem);
	}
#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OBJ_CORE)
	check(live_count() == 0, "provider maximum deinit unlinks object-core node");
#elif !defined(CONFIG_OVE_RTOS_ZEPHYR)
	check(live_count() == 0, "provider maximum deinit releases native resources");
#endif
	sem = NULL;
	private_cleanup();
	ove_sem_storage_t empty = {0};
	if (ove_sem_init(&sem, &empty, 0, 2))
		reject();
	ove_sem_give(sem);
	if (ove_sem_take(sem, 0))
		reject();
	ove_sem_deinit(sem);
#if defined(CONFIG_OVE_RTOS_ZEPHYR) && defined(CONFIG_OBJ_CORE)
	check(live_count() == 0, "zero-initial control unlinks object-core node");
#elif !defined(CONFIG_OVE_RTOS_ZEPHYR)
	check(live_count() == 0, "zero-initial control releases native resources");
#endif
	private_cleanup();
}

#ifdef CONFIG_OVE_RTOS_NUTTX
static const char *status_name(int status)
{
	if (status == OVE_OK)
		return "OK";
	if (status == OVE_ERR_TIMEOUT)
		return "TIMEOUT";
	if (status == OVE_ERR_BUSY)
		return "BUSY";
	if (status == OVE_ERR_IO)
		return "IO";
	return "OTHER";
}

static void cv_case(unsigned selected, union backing *storage)
{
	const bool void_call = selected == 29 || selected == 30;
	ove_condvar_t cv = NULL;
	ove_mutex_storage_t caller = {0};
	ove_mutex_t mutex = NULL;
	if (ove_condvar_init(&cv, &storage->cv))
		reject();
	if (!void_call && (ove_mutex_init(&mutex, &caller) || ove_mutex_lock(mutex, 0)))
		reject();
	if (void_call)
		cv->nwaiters = 1;
	cv_observer = (__typeof__(cv_observer)){
		.cv = cv, .caller = void_call ? NULL : &caller.mtx, .active = true, .error = -EIO};
	int expected = OVE_ERR_IO;
	if (selected >= 14 && selected <= 16)
		cv_observer.failure_role = selected - 13;
	else if (selected >= 20 && selected <= 22) {
		cv_observer.failure_role = selected - 19;
		cv_observer.error = -ETIMEDOUT;
		expected = OVE_ERR_BUSY;
	} else if (selected >= 26 && selected <= 28) {
		cv_observer.failure_role = selected - 25;
		cv_observer.error = -EINTR;
		expected = OVE_ERR_TIMEOUT;
	} else if (void_call)
		cv_observer.failure_role = CV_ENTRY;
	else if (selected == 23)
		expected = OVE_ERR_TIMEOUT;
	else if (selected == 24) {
		expected = OVE_OK;
		cv_observer.signal_token = true;
	} else if (selected == 25) {
		expected = OVE_ERR_BUSY;
		wait_error = -ETIMEDOUT;
	}
	if (selected == 17)
		wait_error = -EIO;
	cv_observer.remaining = cv_observer.failure_role != 0;
	int rc = OVE_OK;
	if (selected == 29)
		ove_condvar_signal(cv);
	else if (selected == 30)
		ove_condvar_broadcast(cv);
	else
		rc = ove_condvar_wait(
			cv, mutex, selected == 24 || selected == 25 ? OVE_WAIT_FOREVER : 1000000);
	const bool failed_entry = selected == 14 || selected == 20;
	const bool failed_caller = selected == 15 || selected == 21;
	const bool failed_retire = selected == 16 || selected == 22;
	int held = void_call ? -1 : (int)find(&caller.mtx)->held;
	int expected_held = void_call ? -1 : failed_caller ? 0 : 1;
	int expected_waiters = void_call ? 1 : 0;
	const char *trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/r0:1:1";
	if (failed_entry)
		trace = "G0:1:0";
	else if (failed_caller)
		trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:0:0/r0:0:1";
	else if (failed_retire)
		trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/R1:1:0/r0:1:1";
	else if (selected == 26)
		trace = "G0:1:0/G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/r0:1:1";
	else if (selected == 27)
		trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/M1:0:0/R1:1:0/r0:1:1";
	else if (selected == 28)
		trace = "G0:1:0/g1:1:1/m1:1:0/W1:0:0/M1:0:0/R1:1:0/R1:1:0/r0:1:1";
	else if (void_call)
		trace = "G1:-1:0";
	if (!void_call)
		check(rc == expected, "condition return class preserves ownership policy");
	check(held == expected_held, "condition caller ownership matches completed reacquisition");
	check(cv->nwaiters == expected_waiters,
	      "condition registration follows acquired bookkeeping guards");
	check(strcmp(cv_observer.trace, trace) == 0,
	      "condition operations preserve role and ownership order");
	check(cv_observer.posts == (unsigned)(selected == 24),
	      "condition posts require a successful guarded signal");
	check(cv_observer.injections == (unsigned)(selected != 23 && selected != 24),
	      "condition injection occurs exactly once at its selected role");
	printf("cv: trace=%s status=%s held=%d waiters=%d posts=%u injections=%u\n",
	       cv_observer.trace, void_call ? "void" : status_name(rc), held, cv->nwaiters,
	       cv_observer.posts, cv_observer.injections);
	cv_observer.active = false;
	cv_observer.cv = NULL;
	cv_observer.caller = NULL;
	fail_lock = 0;
	wait_error = 0;
	lock_error = -EIO;
	mutex = NULL;
	cv = NULL;
	private_cleanup();
	reuse_backing(CONDVAR, storage);
}
#endif

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	static const char *names[] = {"mutex_init",
				      "recursive_init",
				      "sem_init",
				      "event_init",
				      "condvar_init",
				      "condvar_second_init",
				      "count_zero",
				      "count_order",
				      "count_initial_domain",
				      "count_max_domain",
				      "mutex_error",
				      "recursive_error",
				      "sem_error",
				      "event_error",
				      "cv_guard_error",
				      "cv_reacquire_error",
				      "cv_retire_error",
				      "cv_wait_error",
				      "controls",
				      "canary",
				      "cv_guard_timeout_error",
				      "cv_reacquire_timeout_error",
				      "cv_retire_timeout_error",
				      "cv_clean_timeout",
				      "cv_success",
				      "cv_forever_timeout_error",
				      "cv_entry_eintr",
				      "cv_reacquire_eintr",
				      "cv_retire_eintr",
				      "cv_signal_guard_error",
				      "cv_broadcast_guard_error",
				      "mutex_forever_error",
				      "recursive_forever_error",
				      "sem_forever_error",
				      "event_forever_error",
				      "count_initial_ceiling_plus_one",
				      "count_max_ceiling_plus_one"};
	int selected = -1;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
		if (!strcmp(names[i], argv[1]))
			selected = (int)i;
	if (selected < 0)
		return 2;
#ifdef CONFIG_OVE_RTOS_FREERTOS
	if ((selected >= 3 && selected <= 5) || (selected >= 8 && selected <= 17) || selected >= 20)
		return 2;
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
	if (selected == 5 || selected == 8 || selected == 9 || (selected >= 14 && selected <= 17) ||
	    (selected >= 20 && selected <= 30) || selected >= 35)
		return 2;
#endif
	if (selected == 19)
		check(0, "explicit early failure canary");
	ordinary_controls();
	union backing storage = {0}, sentinel = {0};
	enum family family = selected <= 4 ? (enum family)selected : CONDVAR;
	union handle original = sentinel_handle(family, &sentinel), out = original;
	if (selected <= 5) {
#ifdef CONFIG_OVE_RTOS_FREERTOS
		if (selected >= 3)
			return 2;
#elif defined(CONFIG_OVE_RTOS_ZEPHYR)
		if (selected == 5)
			return 2;
#endif
		fail_init = init_calls + (selected == 5 ? 2 : 1);
		int rc = initialize(family, &storage, &out, 0, 1);
		check(rc == INIT_FAILURE, "constructor preserves native failure");
		check(same_handle(family, out, original), "constructor error preserves output");
		check(live_count() == 0, "constructor error releases successful partial pieces");
		out = (union handle){0};
		private_cleanup();
		fail_init = 0;
		reuse_backing(family, &storage);
		reuse_backing(family, &storage);
	} else if ((selected >= 6 && selected <= 9) || selected >= 35) {
		family = SEMAPHORE;
		original = sentinel_handle(family, &sentinel);
		out = original;
		unsigned initial = selected == 7 ? 2 : selected == 8 ? UINT_MAX : 0;
		unsigned maximum = selected == 6 ? 0 : selected == 7 ? 1 : UINT_MAX;
		if (selected >= 35) {
			maximum = (unsigned)INT_MAX + 1u;
			initial = selected == 35 ? maximum : 0;
		}
#if !defined(CONFIG_OVE_RTOS_NUTTX)
		if (selected >= 8)
			return 2;
#endif
		unsigned before = init_calls;
		union backing bytes;
		memcpy(&bytes, &storage, sizeof(bytes));
		int rc = initialize(family, &storage, &out, initial, maximum);
		check(rc == OVE_ERR_INVALID_PARAM, "invalid count is rejected");
		check(same_handle(family, out, original), "invalid count preserves output");
		check(init_calls == before, "invalid count never enters the provider");
		check(memcmp(&bytes, &storage, sizeof(bytes)) == 0,
		      "invalid count preserves backing");
		out = (union handle){0};
		private_cleanup();
		reuse_backing(family, &storage);
		reuse_backing(family, &storage);
	} else if ((selected >= 14 && selected <= 17) || (selected >= 20 && selected <= 30)) {
#ifdef CONFIG_OVE_RTOS_NUTTX
		cv_case((unsigned)selected, &storage);
#else
		return 2;
#endif
	} else if ((selected >= 10 && selected <= 13) || (selected >= 31 && selected <= 34)) {
#ifdef CONFIG_OVE_RTOS_FREERTOS
		return 2;
#endif
		family = (enum family)(selected >= 31 ? selected - 31 : selected - 10);
		out = (union handle){0};
		if (initialize(family, &storage, &out, 0, 1))
			reject();
		int rc;
		uint64_t timeout = selected >= 31 ? OVE_WAIT_FOREVER : 0;
		if (family <= RECURSIVE) {
			fail_lock = lock_calls + 1;
			lock_error = -EIO;
			rc = family == NORMAL ? ove_mutex_lock(out.mutex, timeout)
					      : ove_recursive_mutex_lock(out.mutex, timeout);
		} else if (family <= EVENT) {
			wait_error = -EIO;
			rc = family == SEMAPHORE ? ove_sem_take(out.sem, timeout)
						 : ove_event_wait(out.event, timeout);
		}
		check(rc == OVE_ERR_IO, "native acquisition preserves non-timeout error");
		out = (union handle){0};
		private_cleanup();
		fail_lock = 0;
		wait_error = 0;
		reuse_backing(family, &storage);
	} else if (selected != 18 && selected != 19) {
		return 2;
	}
	check(live_count() == 0, "all model resources retired while backing lives");
	printf("%s: failures=%u controls=%u init_attempts=%u safety=%u private_repairs=%u\n",
	       argv[1], failures, controls, init_calls, safety, private_repairs);
	return failures ? 1 : 0;
}
