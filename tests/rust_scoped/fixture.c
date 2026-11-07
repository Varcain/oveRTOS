/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
#include "ove/storage.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MUTEX, RECURSIVE, CONDVAR, KINDS };
enum {
	INIT_ENTRY,
	CREATE_ENTRY,
	DEINIT_ENTRY,
	DESTROY_ENTRY,
	DUPLICATE,
	FORBIDDEN,
	NATIVE_INIT,
	NATIVE_DEINIT,
	LOCK_ENTRY,
	UNLOCK_ENTRY,
	WAIT_ENTRY,
	NOTIFY_ENTRY,
	OWNER_RETIRES,
	COUNTERS
};
enum { UNUSED, INITIALIZING, LIVE, TEARING_DOWN };
enum { INIT_STAGE, DEINIT_STAGE, LOCK_STAGE, WAIT_RETURN_STAGE, STAGES };
struct slot {
	void *backing;
	uintptr_t key;
	unsigned kind, state, generation, depth, waiters;
	bool heap, owner_valid;
	pthread_t owner;
};
static struct slot slots[128];
static unsigned next_slot, counts[KINDS][COUNTERS], failure[KINDS];
static bool gates[KINDS][STAGES], entered[KINDS][STAGES];
static bool recursive_model;
static unsigned wait_model;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local uint32_t context_override = UINT32_MAX;
static _Thread_local int isr_override = -1;
static _Thread_local bool creating[KINDS];
static _Atomic unsigned clock_calls;

int native_mutex_init(ove_mutex_t *, ove_mutex_storage_t *);
void native_mutex_deinit(ove_mutex_t);
int native_mutex_lock(ove_mutex_t, uint64_t);
void native_mutex_unlock(ove_mutex_t);
int native_recursive_mutex_init(ove_mutex_t *, ove_mutex_storage_t *);
void native_recursive_mutex_deinit(ove_mutex_t);
int native_recursive_mutex_lock(ove_mutex_t, uint64_t);
void native_recursive_mutex_unlock(ove_mutex_t);
int native_condvar_init(ove_condvar_t *, ove_condvar_storage_t *);
void native_condvar_deinit(ove_condvar_t);
int native_condvar_wait_checked(ove_condvar_t, ove_mutex_t, uint64_t, bool *);
void native_condvar_signal(ove_condvar_t);
void native_condvar_broadcast(ove_condvar_t);
uint32_t native_sync_context(void);
bool native_is_in_isr(void);
#ifdef OVE_HEAP_SYNC
int native_mutex_create(ove_mutex_t *);
void native_mutex_destroy(ove_mutex_t);
int native_recursive_mutex_create(ove_mutex_t *);
void native_recursive_mutex_destroy(ove_mutex_t);
int native_condvar_create(ove_condvar_t *);
void native_condvar_destroy(ove_condvar_t);
#endif

static void reject(void)
{
	fputs("fixture rejected: invalid ownership observer setup\n", stderr);
	exit(2);
}

uint32_t ove_sync_context(void)
{
	return context_override == UINT32_MAX ? native_sync_context() : context_override;
}
bool ove_is_in_isr(void)
{
	return isr_override < 0 ? native_is_in_isr() : isr_override != 0;
}
void scoped_test_context(uint32_t flags, int isr)
{
	context_override = flags;
	isr_override = isr;
}
#ifdef CONFIG_OVE_TIME
int ove_time_get_ns(uint64_t *out)
{
	if (out == NULL)
		return OVE_ERR_INVALID_PARAM;
	atomic_fetch_add(&clock_calls, 1);
	*out = 1000;
	return OVE_OK;
}
#endif
unsigned scoped_test_clock_calls(void)
{
	return atomic_load(&clock_calls);
}
static bool resource_ok(void)
{
	return !ove_is_in_isr() && (ove_sync_context() & OVE_SYNC_CONTEXT_RESOURCE) != 0;
}
static bool blocking_ok(void)
{
	return !ove_is_in_isr() && (ove_sync_context() & OVE_SYNC_CONTEXT_BLOCKING) != 0;
}
static struct slot *find(void *backing, unsigned kind)
{
	for (unsigned i = 0; i < next_slot; ++i)
		if (slots[i].state != UNUSED && slots[i].key == (uintptr_t)backing &&
		    slots[i].kind == kind)
			return &slots[i];
	return NULL;
}
static bool owned(const struct slot *slot)
{
	return slot->owner_valid && pthread_equal(slot->owner, pthread_self());
}
static void clear_owner(struct slot *slot)
{
	slot->depth = 0;
	slot->owner_valid = false;
	memset(&slot->owner, 0, sizeof(slot->owner));
}
static void pause_at(unsigned kind, unsigned stage)
{
	bool first = !entered[kind][stage];
	entered[kind][stage] = true;
	pthread_cond_broadcast(&changed);
	while (first && gates[kind][stage])
		pthread_cond_wait(&changed, &registry);
}
void scoped_test_gate(unsigned kind, unsigned stage, bool enabled)
{
	if (kind >= KINDS || stage >= STAGES)
		reject();
	pthread_mutex_lock(&registry);
	gates[kind][stage] = enabled;
	if (enabled)
		entered[kind][stage] = false;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&registry);
}
void scoped_test_wait_entered(unsigned kind, unsigned stage)
{
	if (kind >= KINDS || stage >= STAGES)
		reject();
	pthread_mutex_lock(&registry);
	while (!entered[kind][stage])
		pthread_cond_wait(&changed, &registry);
	pthread_mutex_unlock(&registry);
}
unsigned scoped_test_count(unsigned kind, unsigned index)
{
	if (kind >= KINDS || index >= COUNTERS)
		reject();
	pthread_mutex_lock(&registry);
	unsigned result = counts[kind][index];
	pthread_mutex_unlock(&registry);
	return result;
}
unsigned scoped_test_live(unsigned kind)
{
	if (kind >= KINDS)
		reject();
	pthread_mutex_lock(&registry);
	unsigned result = 0;
	for (unsigned i = 0; i < next_slot; ++i)
		result += slots[i].kind == kind && slots[i].state != UNUSED;
	pthread_mutex_unlock(&registry);
	return result;
}
unsigned scoped_test_depth(unsigned kind)
{
	pthread_mutex_lock(&registry);
	unsigned result = 0;
	for (unsigned i = 0; i < next_slot; ++i)
		if (slots[i].kind == kind && slots[i].state == LIVE && owned(&slots[i]))
			result += slots[i].depth;
	pthread_mutex_unlock(&registry);
	return result;
}
bool scoped_test_registry_ok(void)
{
	bool result = true;
	pthread_mutex_lock(&registry);
	for (unsigned i = 0; i < next_slot; ++i) {
		const struct slot *slot = &slots[i];
		result &= slot->generation == i + 1;
		result &= (slot->depth != 0) == slot->owner_valid;
		result &= slot->state == UNUSED
				  ? slot->backing == NULL
				  : slot->backing != NULL && slot->key == (uintptr_t)slot->backing;
		for (unsigned j = 0; j < i; ++j)
			if (slot->state != UNUSED && slots[j].state != UNUSED &&
			    slot->key == slots[j].key)
				result = false;
	}
	pthread_mutex_unlock(&registry);
	return result;
}
void scoped_test_failure(unsigned kind, unsigned mode)
{
	if (kind >= KINDS || mode > 2)
		reject();
	pthread_mutex_lock(&registry);
	failure[kind] = mode;
	pthread_mutex_unlock(&registry);
}
void scoped_test_recursive_model(bool enabled)
{
	pthread_mutex_lock(&registry);
	recursive_model = enabled;
	pthread_mutex_unlock(&registry);
}
void scoped_test_wait_model(unsigned mode)
{
	if (mode > 2)
		reject();
	pthread_mutex_lock(&registry);
	wait_model = mode;
	pthread_mutex_unlock(&registry);
}

static int begin_init(unsigned kind, void *backing, struct slot **out)
{
	pthread_mutex_lock(&registry);
	++counts[kind][INIT_ENTRY];
	if (!resource_ok()) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (find(backing, kind) != NULL) {
		++counts[kind][DUPLICATE];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_BUSY;
	}
	if (failure[kind] == 1) {
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NO_MEMORY;
	}
	if (next_slot == sizeof(slots) / sizeof(slots[0]))
		reject();
	struct slot *slot = &slots[next_slot++];
	*slot = (struct slot){.backing = backing,
			      .key = (uintptr_t)backing,
			      .kind = kind,
			      .state = INITIALIZING,
			      .generation = next_slot,
			      .heap = creating[kind]};
	*out = slot;
	pause_at(kind, INIT_STAGE);
	pthread_mutex_unlock(&registry);
	return OVE_OK;
}
static int finish_init(unsigned kind, struct slot *slot, int status)
{
	pthread_mutex_lock(&registry);
	if (status == OVE_OK) {
		++counts[kind][NATIVE_INIT];
		slot->state = LIVE;
	} else {
		slot->state = UNUSED;
		slot->backing = NULL;
	}
	unsigned mode = failure[kind];
	pthread_mutex_unlock(&registry);
	return status == OVE_OK && mode == 2 ? OVE_ERR_NO_MEMORY : status;
}
static struct slot *begin_deinit(unsigned kind, void *backing)
{
	pthread_mutex_lock(&registry);
	++counts[kind][DEINIT_ENTRY];
	struct slot *slot = find(backing, kind);
	if (!resource_ok() || slot == NULL || slot->state != LIVE || slot->depth || slot->waiters) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return NULL;
	}
	slot->state = TEARING_DOWN;
	pause_at(kind, DEINIT_STAGE);
	pthread_mutex_unlock(&registry);
	return slot;
}
static void finish_deinit(unsigned kind, struct slot *slot)
{
	pthread_mutex_lock(&registry);
	++counts[kind][NATIVE_DEINIT];
	slot->state = UNUSED;
	slot->backing = NULL;
	pthread_mutex_unlock(&registry);
}

#define INIT_WRAPPER(name, kind, handle_type, storage_type)            \
	int ove_##name##_init(handle_type *out, storage_type *backing) \
	{                                                              \
		struct slot *slot = NULL;                              \
		int rc = begin_init(kind, backing, &slot);             \
		if (rc != OVE_OK)                                      \
			return rc;                                     \
		rc = native_##name##_init(out, backing);               \
		int reported = finish_init(kind, slot, rc);            \
		if (rc == OVE_OK && reported != OVE_OK)                \
			ove_##name##_deinit(*out);                     \
		return reported;                                       \
	}                                                              \
	void ove_##name##_deinit(handle_type handle)                   \
	{                                                              \
		struct slot *slot = begin_deinit(kind, handle);        \
		if (slot != NULL) {                                    \
			native_##name##_deinit(handle);                \
			finish_deinit(kind, slot);                     \
		}                                                      \
	}
INIT_WRAPPER(mutex, MUTEX, ove_mutex_t, ove_mutex_storage_t)
INIT_WRAPPER(recursive_mutex, RECURSIVE, ove_mutex_t, ove_mutex_storage_t)
INIT_WRAPPER(condvar, CONDVAR, ove_condvar_t, ove_condvar_storage_t)

static int lock(unsigned kind, ove_mutex_t handle, uint64_t timeout)
{
	pthread_mutex_lock(&registry);
	++counts[kind][LOCK_ENTRY];
	struct slot *slot = find(handle, kind);
	if (!blocking_ok() || slot == NULL || slot->state != LIVE) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NOT_SUPPORTED;
	}
	pause_at(kind, LOCK_STAGE);
	if (kind == MUTEX && recursive_model && owned(slot)) {
		if (slot->depth >= 4)
			reject();
		++slot->depth;
		pthread_mutex_unlock(&registry);
		return OVE_OK;
	}
	pthread_mutex_unlock(&registry);
	int rc = kind == MUTEX ? native_mutex_lock(handle, timeout)
			       : native_recursive_mutex_lock(handle, timeout);
	if (rc == OVE_OK) {
		pthread_mutex_lock(&registry);
		if (slot->depth != 0 && (kind == MUTEX || !owned(slot)))
			reject();
		++slot->depth;
		slot->owner = pthread_self();
		slot->owner_valid = true;
		pthread_mutex_unlock(&registry);
	}
	return rc;
}
static void unlock(unsigned kind, ove_mutex_t handle)
{
	pthread_mutex_lock(&registry);
	++counts[kind][UNLOCK_ENTRY];
	struct slot *slot = find(handle, kind);
	if (ove_is_in_isr() || slot == NULL || slot->state != LIVE || !owned(slot) ||
	    !slot->depth) {
		++counts[kind][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return;
	}
	if (kind == MUTEX && recursive_model && slot->depth > 1) {
		--slot->depth;
	} else {
		--slot->depth;
		if (slot->depth == 0)
			clear_owner(slot);
		if (kind == MUTEX)
			native_mutex_unlock(handle);
		else
			native_recursive_mutex_unlock(handle);
	}
	pthread_mutex_unlock(&registry);
}
int ove_mutex_lock(ove_mutex_t handle, uint64_t timeout)
{
	return lock(MUTEX, handle, timeout);
}
void ove_mutex_unlock(ove_mutex_t handle)
{
	unlock(MUTEX, handle);
}
int ove_recursive_mutex_lock(ove_mutex_t handle, uint64_t timeout)
{
	return lock(RECURSIVE, handle, timeout);
}
void ove_recursive_mutex_unlock(ove_mutex_t handle)
{
	unlock(RECURSIVE, handle);
}

int ove_condvar_wait_checked(ove_condvar_t cv, ove_mutex_t mutex, uint64_t timeout, bool *held)
{
	if (held == NULL)
		return OVE_ERR_INVALID_PARAM;
	*held = mutex != NULL;
	if (cv == NULL || mutex == NULL)
		return OVE_ERR_INVALID_PARAM;
	pthread_mutex_lock(&registry);
	++counts[CONDVAR][WAIT_ENTRY];
	struct slot *c = find(cv, CONDVAR), *m = find(mutex, MUTEX);
	if (!blocking_ok() || c == NULL || c->state != LIVE || m == NULL || m->state != LIVE ||
	    !owned(m) || m->depth != 1 || held == NULL) {
		++counts[CONDVAR][FORBIDDEN];
		pthread_mutex_unlock(&registry);
		if (held != NULL)
			*held = true;
		return OVE_ERR_NOT_SUPPORTED;
	}
	*held = true;
	unsigned mode = wait_model;
	if (mode == 1) {
		pthread_mutex_unlock(&registry);
		return OVE_ERR_IO;
	}
	clear_owner(m);
	if (mode == 2) {
		native_mutex_unlock(mutex);
		*held = false;
		pause_at(CONDVAR, WAIT_RETURN_STAGE);
		pthread_mutex_unlock(&registry);
		return OVE_ERR_IO;
	}
	++c->waiters;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&registry);
	int rc = native_condvar_wait_checked(cv, mutex, timeout, held);
	pthread_mutex_lock(&registry);
	--c->waiters;
	if (*held) {
		if (m->depth != 0)
			reject();
		m->depth = 1;
		m->owner = pthread_self();
		m->owner_valid = true;
	}
	pthread_mutex_unlock(&registry);
	return rc;
}
int ove_condvar_wait(ove_condvar_t cv, ove_mutex_t mutex, uint64_t timeout)
{
	bool held;
	return ove_condvar_wait_checked(cv, mutex, timeout, &held);
}
static void notify(ove_condvar_t cv, bool all)
{
	pthread_mutex_lock(&registry);
	++counts[CONDVAR][NOTIFY_ENTRY];
	struct slot *slot = find(cv, CONDVAR);
	bool valid = blocking_ok() && slot != NULL && slot->state == LIVE;
	if (!valid)
		++counts[CONDVAR][FORBIDDEN];
	pthread_mutex_unlock(&registry);
	if (valid) {
		if (all)
			native_condvar_broadcast(cv);
		else
			native_condvar_signal(cv);
	}
}
void ove_condvar_signal(ove_condvar_t cv)
{
	notify(cv, false);
}
void ove_condvar_broadcast(ove_condvar_t cv)
{
	notify(cv, true);
}

#ifdef OVE_HEAP_SYNC
static bool heap_entry(unsigned kind, unsigned index, void *handle)
{
	pthread_mutex_lock(&registry);
	++counts[kind][index];
	bool valid = resource_ok();
	if (index == DESTROY_ENTRY) {
		struct slot *slot = find(handle, kind);
		valid = valid && slot != NULL && slot->state == LIVE && !slot->depth &&
			!slot->waiters;
	}
	if (!valid)
		++counts[kind][FORBIDDEN];
	pthread_mutex_unlock(&registry);
	return valid;
}
#define HEAP_WRAPPER(name, kind, handle_type)                \
	int ove_##name##_create(handle_type *out)            \
	{                                                    \
		if (!heap_entry(kind, CREATE_ENTRY, NULL))   \
			return OVE_ERR_NOT_SUPPORTED;        \
		creating[kind] = true;                       \
		int rc = native_##name##_create(out);        \
		creating[kind] = false;                      \
		return rc;                                   \
	}                                                    \
	void ove_##name##_destroy(handle_type handle)        \
	{                                                    \
		if (heap_entry(kind, DESTROY_ENTRY, handle)) \
			native_##name##_destroy(handle);     \
	}
HEAP_WRAPPER(mutex, MUTEX, ove_mutex_t)
HEAP_WRAPPER(recursive_mutex, RECURSIVE, ove_mutex_t)
HEAP_WRAPPER(condvar, CONDVAR, ove_condvar_t)
#endif

/* Only the observed current owner can retire an acquisition, before its worker
 * returns. These independent safety effects never supply a passing observation. */
unsigned scoped_test_release_current(void)
{
	unsigned result = 0;
	pthread_mutex_lock(&registry);
	for (unsigned i = 0; i < next_slot; ++i) {
		struct slot *slot = &slots[i];
		if (slot->state != LIVE || !owned(slot) || !slot->depth)
			continue;
		unsigned depth = slot->depth;
		clear_owner(slot);
		if (slot->kind == MUTEX)
			native_mutex_unlock(slot->backing);
		else if (slot->kind == RECURSIVE)
			for (unsigned j = 0; j < depth; ++j)
				native_recursive_mutex_unlock(slot->backing);
		else
			reject();
		++counts[slot->kind][OWNER_RETIRES];
		++result;
	}
	pthread_mutex_unlock(&registry);
	return result;
}

/* Called only after every Rust owner/capability is gone and workers are joined.
 * Retention is observed separately before this independent harness cleanup. */
unsigned scoped_test_cleanup_retained(void)
{
	unsigned result = 0;
	pthread_mutex_lock(&registry);
	for (unsigned i = 0; i < next_slot; ++i) {
		struct slot *slot = &slots[i];
		if (slot->state == UNUSED)
			continue;
		if (slot->state != LIVE || slot->depth || slot->waiters)
			reject();
		void *backing = slot->backing;
		if (slot->kind == MUTEX)
			native_mutex_deinit(backing);
		else if (slot->kind == RECURSIVE)
			native_recursive_mutex_deinit(backing);
		else
			native_condvar_deinit(backing);
		slot->state = UNUSED;
		slot->backing = NULL;
		++counts[slot->kind][NATIVE_DEINIT];
#ifdef OVE_HEAP_SYNC
		if (slot->heap)
			free(backing);
#endif
		++result;
	}
	pthread_mutex_unlock(&registry);
	return result;
}

void scoped_test_wait_registered(void)
{
	pthread_mutex_lock(&registry);
	bool waiting = false;
	while (!waiting) {
		for (unsigned i = 0; i < next_slot; ++i)
			waiting |= slots[i].state == LIVE && slots[i].kind == CONDVAR &&
				   slots[i].waiters != 0;
		if (!waiting)
			pthread_cond_wait(&changed, &registry);
	}
	pthread_mutex_unlock(&registry);
}
