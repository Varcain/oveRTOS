/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <ove/ove.h>
#include <ove/storage.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { SEMAPHORE, EVENT, KINDS };
enum {
	INIT_ENTRY,
	CREATE_ENTRY,
	DEINIT_ENTRY,
	DESTROY_ENTRY,
	DUPLICATE,
	FORBIDDEN,
	NATIVE_INIT,
	NATIVE_DEINIT,
	WAIT_ENTRY,
	TASK_POST,
	ISR_POST,
	NOTIFY_ENTRY,
	HARNESS_CLEANUP,
	COUNTERS
};
enum { UNUSED, INITIALIZING, LIVE, TEARING_DOWN };
struct slot {
	void *backing;
	uintptr_t backing_key;
	unsigned int generation;
	unsigned int kind;
	unsigned int state;
	unsigned int active;
	unsigned int fault;
	bool heap;
};
static struct slot slots[64];
static size_t next_slot;
static unsigned int counts[KINDS][COUNTERS];
static unsigned int failure[KINDS];
static bool held[KINDS][2];
static bool entered[KINDS][2];
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local uint32_t context_override = UINT32_MAX;
static _Thread_local bool creating[KINDS];
static _Thread_local struct slot *destroying[KINDS];
static unsigned int clock_calls, forbidden_clock_calls;

int native_sem_init(ove_sem_t *, ove_sem_storage_t *, unsigned int, unsigned int);
void native_sem_deinit(ove_sem_t);
int native_sem_take(ove_sem_t, uint64_t);
void native_sem_give(ove_sem_t);
int native_sem_set_notify(ove_sem_t, ove_notify_cb, void *);
uint32_t native_sync_context(void);
bool native_is_in_isr(void);
void posix_irq_enter_isr(void);
void posix_irq_leave_isr(void);
uint64_t sync_test_actual_critical_depth(void);
int native_event_init(ove_event_t *, ove_event_storage_t *);
void native_event_deinit(ove_event_t);
int native_event_wait(ove_event_t, uint64_t);
void native_event_signal(ove_event_t);
void native_event_signal_from_isr(ove_event_t);
#ifdef OVE_HEAP_SYNC
int native_sem_create(ove_sem_t *, unsigned int, unsigned int);
void native_sem_destroy(ove_sem_t);
int native_event_create(ove_event_t *);
void native_event_destroy(ove_event_t);
#endif

/* Context overrides model source branches. Ordinary controls use the actual
 * POSIX query and lock depth; no physical signal handler runs these primitives. */
static void reject(const char *message)
{
	fprintf(stderr, "fixture rejected: %s\n", message);
	exit(2);
}

uint32_t ove_sync_context(void)
{
	if (native_is_in_isr())
		return 0;
	return context_override == UINT32_MAX ? native_sync_context() : context_override;
}

bool ove_is_in_isr(void)
{
	return native_is_in_isr();
}

void sync_test_interrupt(bool value)
{
	if (value && !native_is_in_isr())
		posix_irq_enter_isr();
	else if (!value && native_is_in_isr())
		posix_irq_leave_isr();
}

void sync_test_context(uint32_t flags)
{
	if (flags != UINT32_MAX && flags != 0 && flags != 1 && flags != 3 && flags != 4 &&
	    flags != 5 && flags != 7)
		reject("unsupported metadata context");
	context_override = flags;
}

/* This independent guard uses supplied metadata or actual TLS depth, rather
 * than the product query's computed answer. It reserves no Rust claim. */
static uint32_t admitted(void)
{
	if (native_is_in_isr())
		return 0;
	if (context_override != UINT32_MAX)
		return context_override;
	return sync_test_actual_critical_depth() ? 4u : 7u;
}

static bool resource_ok(void)
{
	return (admitted() & 1u) != 0;
}

static bool blocking_ok(void)
{
	return (admitted() & 2u) != 0;
}

static bool producer_ok(void)
{
	return (admitted() & 4u) != 0;
}

unsigned sync_test_clock_calls(void)
{
	pthread_mutex_lock(&registry);
	unsigned value = clock_calls;
	pthread_mutex_unlock(&registry);
	return value;
}

unsigned sync_test_forbidden_clock_calls(void)
{
	pthread_mutex_lock(&registry);
	unsigned value = forbidden_clock_calls;
	pthread_mutex_unlock(&registry);
	return value;
}

#ifdef CONFIG_OVE_TIME
/* A valid deterministic clock isolates deadline entry. Rejected calls never
 * read a native clock or write an uninitialized caller output. */
int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL)
		return OVE_ERR_INVALID_PARAM;
	pthread_mutex_lock(&registry);
	++clock_calls;
	bool valid = blocking_ok();
	if (!valid)
		++forbidden_clock_calls;
	pthread_mutex_unlock(&registry);
	if (!valid)
		return OVE_ERR_NOT_SUPPORTED;
	*out = 1000;
	return OVE_OK;
}
#endif

unsigned int sync_test_count(unsigned int kind, unsigned int index)
{
	unsigned int value = 0;
	pthread_mutex_lock(&registry);
	if (kind < KINDS && index < COUNTERS)
		value = counts[kind][index];
	pthread_mutex_unlock(&registry);
	return value;
}

unsigned int sync_test_live(unsigned int kind)
{
	unsigned int value = 0;
	pthread_mutex_lock(&registry);
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
		if (slots[i].state != UNUSED && slots[i].kind == kind)
			++value;
	pthread_mutex_unlock(&registry);
	return value;
}

bool sync_test_registry_ok(void)
{
	bool valid = true;
	pthread_mutex_lock(&registry);
	for (unsigned kind = 0; kind < KINDS; ++kind)
		for (unsigned stage = 0; stage < 2; ++stage)
			valid = valid && !held[kind][stage];
	for (size_t i = 0; i < next_slot; ++i) {
		valid = valid && slots[i].generation == i + 1;
		if (slots[i].state == UNUSED) {
			valid = valid && slots[i].backing == NULL && slots[i].active == 0;
		} else {
			valid = valid && slots[i].state == LIVE && slots[i].active == 0;
			valid = valid && slots[i].backing_key == (uintptr_t)slots[i].backing;
			for (size_t j = 0; j < i; ++j)
				if (slots[j].state != UNUSED &&
				    slots[j].backing_key == slots[i].backing_key)
					valid = false;
		}
	}
	pthread_mutex_unlock(&registry);
	return valid;
}

void sync_test_failure(unsigned int kind, unsigned int value)
{
	pthread_mutex_lock(&registry);
	if (kind < KINDS)
		failure[kind] = value;
	pthread_mutex_unlock(&registry);
}

void sync_test_gate(unsigned int kind, unsigned int stage, bool value)
{
	pthread_mutex_lock(&registry);
	if (kind < KINDS && stage < 2) {
		held[kind][stage] = value;
		if (value)
			entered[kind][stage] = false;
	}
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&registry);
}

bool sync_test_entered(unsigned int kind, unsigned int stage)
{
	bool value = false;
	pthread_mutex_lock(&registry);
	if (kind < KINDS && stage < 2)
		value = entered[kind][stage];
	pthread_mutex_unlock(&registry);
	return value;
}

static void pause_at(unsigned int kind, unsigned int stage)
{
	bool selected = !entered[kind][stage];
	entered[kind][stage] = true;
	pthread_cond_broadcast(&changed);
	/* One admitted visitor owns the latch. An independent heap object's
	 * operation must remain able to complete while that visitor is held. */
	while (selected && held[kind][stage])
		pthread_cond_wait(&changed, &registry);
}

static struct slot *find_slot(void *backing, unsigned int kind)
{
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i)
		if (slots[i].state != UNUSED && slots[i].backing == backing &&
		    slots[i].kind == kind)
			return &slots[i];
	return NULL;
}

static int begin_init(unsigned int kind, void *backing, struct slot **reservation)
{
	pthread_mutex_lock(&registry);
	++counts[kind][INIT_ENTRY];
	if (!resource_ok()) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (find_slot(backing, kind) != NULL) {
		++counts[kind][DUPLICATE];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_BUSY;
	}
	if (failure[kind] == 1) {
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NO_MEMORY;
	}
	if (next_slot < sizeof(slots) / sizeof(slots[0])) {
		/* Never recycle a registry generation. A later allocation at an
		 * earlier address has a fresh record after completed cleanup. */
		struct slot *slot = &slots[next_slot++];
		*slot = (struct slot){.backing = backing,
				      .backing_key = (uintptr_t)backing,
				      .generation = (unsigned)next_slot,
				      .kind = kind,
				      .state = INITIALIZING,
				      .fault = failure[kind],
				      .heap = creating[kind]};
		*reservation = slot;
		pause_at(kind, 0);
		pthread_mutex_unlock(&registry);
		return OVE_OK;
	}
	pthread_mutex_unlock(&registry);
	reject("registry capacity");
	return OVE_ERR_NO_MEMORY;
}

static int finish_init(unsigned int kind, struct slot *slot, int status)
{
	pthread_mutex_lock(&registry);
	if (status == OVE_OK) {
		++counts[kind][NATIVE_INIT];
		slot->state = LIVE;
	} else {
		slot->state = UNUSED;
		slot->backing = NULL;
	}
	unsigned int selected = slot->fault;
	pthread_mutex_unlock(&registry);
	return selected == 2 && status == OVE_OK ? OVE_ERR_NO_MEMORY : status;
}

static struct slot *operation(unsigned kind, void *backing, unsigned index, bool from_isr)
{
	pthread_mutex_lock(&registry);
	++counts[kind][index];
	struct slot *slot = find_slot(backing, kind);
	bool context = from_isr ? native_is_in_isr()
				: (index == WAIT_ENTRY ? blocking_ok() : producer_ok());
	bool valid = slot != NULL && slot->state == LIVE && context;
	if (!valid) {
		++counts[kind][FORBIDDEN];
		slot = NULL;
	} else {
		++slot->active;
	}
	pthread_mutex_unlock(&registry);
	return slot;
}

static void finish_operation(struct slot *slot)
{
	pthread_mutex_lock(&registry);
	if (slot->state != LIVE || slot->active == 0)
		reject("operation reservation");
	--slot->active;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&registry);
}

static struct slot *begin_deinit(unsigned kind, void *backing)
{
	pthread_mutex_lock(&registry);
	++counts[kind][DEINIT_ENTRY];
	struct slot *slot = find_slot(backing, kind);
	bool internal = slot != NULL && slot == destroying[kind];
	bool valid = resource_ok() && slot != NULL && slot->active == 0 &&
		     (slot->state == LIVE || (internal && slot->state == TEARING_DOWN));
	if (!valid) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return NULL;
	}
	if (internal) {
		destroying[kind] = NULL;
	} else {
		slot->state = TEARING_DOWN;
		pause_at(kind, 1);
	}
	pthread_mutex_unlock(&registry);
	return slot;
}

static void finish_deinit(unsigned int kind, struct slot *slot)
{
	pthread_mutex_lock(&registry);
	++counts[kind][NATIVE_DEINIT];
	slot->state = UNUSED;
	/* Do not retain pointer values after the common heap destroy frees the
	 * allocation. Historical addresses are integer snapshots taken while live. */
	slot->backing = NULL;
	pthread_mutex_unlock(&registry);
}

int ove_sem_init(ove_sem_t *out, ove_sem_storage_t *backing, unsigned int initial, unsigned int max)
{
	struct slot *slot = NULL;
	int rc = begin_init(SEMAPHORE, backing, &slot);
	if (rc != OVE_OK)
		return rc;
	rc = native_sem_init(out, backing, initial, max);
	int reported = finish_init(SEMAPHORE, slot, rc);
	if (rc == OVE_OK && reported != OVE_OK) {
		/* Synthetic cleaned error output: valid init/deinit completes first.
		 * The initialized out value is deliberately retained, never used. */
		ove_sem_deinit(*out);
	}
	return reported;
}

int ove_event_init(ove_event_t *out, ove_event_storage_t *backing)
{
	struct slot *slot = NULL;
	int rc = begin_init(EVENT, backing, &slot);
	if (rc != OVE_OK)
		return rc;
	rc = native_event_init(out, backing);
	int reported = finish_init(EVENT, slot, rc);
	if (rc == OVE_OK && reported != OVE_OK)
		ove_event_deinit(*out);
	return reported;
}

void ove_sem_deinit(ove_sem_t handle)
{
	struct slot *slot = begin_deinit(SEMAPHORE, handle);
	if (slot != NULL) {
		native_sem_deinit(handle);
		finish_deinit(SEMAPHORE, slot);
	}
}

void ove_event_deinit(ove_event_t handle)
{
	struct slot *slot = begin_deinit(EVENT, handle);
	if (slot != NULL) {
		native_event_deinit(handle);
		finish_deinit(EVENT, slot);
	}
}

int ove_sem_take(ove_sem_t handle, uint64_t timeout)
{
	struct slot *slot = operation(SEMAPHORE, handle, WAIT_ENTRY, false);
	if (slot == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	int result = native_sem_take(handle, timeout);
	finish_operation(slot);
	return result;
}

void ove_sem_give(ove_sem_t handle)
{
	struct slot *slot = operation(SEMAPHORE, handle, TASK_POST, false);
	if (slot != NULL) {
		native_sem_give(handle);
		finish_operation(slot);
	}
}

#ifdef CONFIG_OVE_ASYNC
int ove_sem_set_notify(ove_sem_t handle, ove_notify_cb callback, void *data)
{
	struct slot *slot = operation(SEMAPHORE, handle, NOTIFY_ENTRY, false);
	if (slot == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	int result = native_sem_set_notify(handle, callback, data);
	finish_operation(slot);
	return result;
}
#endif

int ove_event_wait(ove_event_t handle, uint64_t timeout)
{
	struct slot *slot = operation(EVENT, handle, WAIT_ENTRY, false);
	if (slot == NULL)
		return OVE_ERR_NOT_SUPPORTED;
	int result = native_event_wait(handle, timeout);
	finish_operation(slot);
	return result;
}

void ove_event_signal(ove_event_t handle)
{
	struct slot *slot = operation(EVENT, handle, TASK_POST, false);
	if (slot != NULL) {
		native_event_signal(handle);
		finish_operation(slot);
	}
}

void ove_event_signal_from_isr(ove_event_t handle)
{
	struct slot *slot = operation(EVENT, handle, ISR_POST, true);
	if (slot != NULL) {
		native_event_signal_from_isr(handle);
		finish_operation(slot);
	}
}

#ifdef OVE_HEAP_SYNC
static bool begin_heap(unsigned kind, unsigned index, void *handle)
{
	pthread_mutex_lock(&registry);
	++counts[kind][index];
	bool valid = resource_ok();
	struct slot *slot = NULL;
	if (index == DESTROY_ENTRY) {
		slot = find_slot(handle, kind);
		valid = valid && slot != NULL && slot->state == LIVE && slot->active == 0;
	}
	if (!valid) {
		++counts[kind][FORBIDDEN];
	} else if (slot != NULL) {
		/* Reserve the entire common deinit/free, not only its inner call. */
		slot->state = TEARING_DOWN;
		destroying[kind] = slot;
		pause_at(kind, 1);
	}
	pthread_mutex_unlock(&registry);
	return valid;
}

int ove_sem_create(ove_sem_t *out, unsigned initial, unsigned max)
{
	if (!begin_heap(SEMAPHORE, CREATE_ENTRY, NULL))
		return OVE_ERR_NOT_SUPPORTED;
	creating[SEMAPHORE] = true;
	int result = native_sem_create(out, initial, max);
	creating[SEMAPHORE] = false;
	return result;
}

void ove_sem_destroy(ove_sem_t handle)
{
	if (begin_heap(SEMAPHORE, DESTROY_ENTRY, handle)) {
		native_sem_destroy(handle);
		if (destroying[SEMAPHORE] != NULL)
			reject("common semaphore cleanup did not consume reservation");
	}
}

int ove_event_create(ove_event_t *out)
{
	if (!begin_heap(EVENT, CREATE_ENTRY, NULL))
		return OVE_ERR_NOT_SUPPORTED;
	creating[EVENT] = true;
	int result = native_event_create(out);
	creating[EVENT] = false;
	return result;
}

void ove_event_destroy(ove_event_t handle)
{
	if (begin_heap(EVENT, DESTROY_ENTRY, handle)) {
		native_event_destroy(handle);
		if (destroying[EVENT] != NULL)
			reject("common event cleanup did not consume reservation");
	}
}
#endif

/* All Rust owners/callback handles are gone and workers joined before entry.
 * Logical retention was already measured. Retirement never reopens Rust claims. */
unsigned sync_test_cleanup_retained(void)
{
	if (!resource_ok())
		reject("retirement context");
	unsigned result = 0;
	for (size_t i = 0; i < next_slot; ++i) {
		pthread_mutex_lock(&registry);
		struct slot *slot = &slots[i];
		if (slot->state == UNUSED) {
			pthread_mutex_unlock(&registry);
			continue;
		}
		if (slot->state != LIVE || slot->active != 0)
			reject("retirement requires quiescent live generation");
		slot->state = TEARING_DOWN;
		void *backing = slot->backing;
		unsigned kind = slot->kind;
		bool heap = slot->heap;
		pthread_mutex_unlock(&registry);
		if (kind == SEMAPHORE)
			native_sem_deinit(backing);
		else
			native_event_deinit(backing);
		pthread_mutex_lock(&registry);
		++counts[kind][NATIVE_DEINIT];
		++counts[kind][HARNESS_CLEANUP];
		slot->state = UNUSED;
		slot->backing = NULL;
		pthread_mutex_unlock(&registry);
#ifdef OVE_HEAP_SYNC
		if (heap)
			free(backing);
#else
		(void)heap;
#endif
		++result;
	}
	return result;
}
