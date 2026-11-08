/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
	INIT,
	CREATE,
	DEINIT,
	DESTROY,
	DUPLICATE,
	FORBIDDEN,
	NATIVE_INIT,
	NATIVE_DEINIT,
	SEND,
	RECEIVE,
	ISR_SEND,
	ISR_RECEIVE,
	COUNT,
	ISR_COUNT,
	CAPACITY,
	NOTIFY,
	TASK_ZERO,
	CLOCK,
	FORBIDDEN_CLOCK,
	RETIRED,
	COUNTERS
};
enum { UNUSED, INITIALIZING, LIVE, TEARING_DOWN };
struct generation {
	ove_queue_t backing;
	unsigned state, active;
	bool heap;
};
static struct generation generations[128];
static unsigned next_generation, counts[COUNTERS], failure;
static bool held[2], entered[2];
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local uint32_t context_override = UINT32_MAX;
static _Thread_local bool creating;
static _Thread_local struct generation *destroying;

int native_queue_init(ove_queue_t *, ove_queue_storage_t *, void *, size_t, unsigned);
void native_queue_deinit(ove_queue_t);
int native_queue_send(ove_queue_t, const void *, uint64_t);
int native_queue_receive(ove_queue_t, void *, uint64_t);
int native_queue_send_from_isr(ove_queue_t, const void *);
int native_queue_receive_from_isr(ove_queue_t, void *);
size_t native_queue_count(ove_queue_t);
size_t native_queue_count_from_isr(ove_queue_t);
size_t native_queue_capacity(ove_queue_t);
int native_queue_set_notify(ove_queue_t, ove_notify_cb, void *);
#ifndef CONFIG_OVE_ZERO_HEAP
int native_queue_create(ove_queue_t *, size_t, unsigned);
void native_queue_destroy(ove_queue_t);
#endif
uint32_t native_sync_context(void);
bool native_is_in_isr(void);
uint64_t queue_test_actual_depth(void);
void posix_irq_enter_isr(void);
void posix_irq_leave_isr(void);

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
void queue_test_context(uint32_t flags)
{
	context_override = flags;
}
void queue_test_interrupt(bool value)
{
	if (value)
		posix_irq_enter_isr();
	else
		posix_irq_leave_isr();
}

/* Independent entry admission uses metadata or actual depth, not the answer
 * computed by the product query. No physical signal handler calls a queue. */
static uint32_t admitted(void)
{
	if (native_is_in_isr())
		return 0;
	if (context_override != UINT32_MAX)
		return context_override;
	return queue_test_actual_depth() ? 4u : 7u;
}
static bool allowed(unsigned bit)
{
	return (admitted() & bit) != 0;
}

unsigned queue_test_count(unsigned index)
{
	pthread_mutex_lock(&registry);
	unsigned value = index < COUNTERS ? counts[index] : UINT32_MAX;
	pthread_mutex_unlock(&registry);
	return value;
}
unsigned queue_test_live(void)
{
	pthread_mutex_lock(&registry);
	unsigned count = 0;
	for (unsigned i = 0; i < next_generation; ++i)
		count += generations[i].state != UNUSED;
	pthread_mutex_unlock(&registry);
	return count;
}
void queue_test_failure(unsigned value)
{
	pthread_mutex_lock(&registry);
	failure = value;
	pthread_mutex_unlock(&registry);
}
void queue_test_gate(unsigned stage, bool value)
{
	if (stage > 1)
		reject("invalid latch");
	pthread_mutex_lock(&registry);
	held[stage] = value;
	if (value)
		entered[stage] = false;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&registry);
}
bool queue_test_entered(unsigned stage)
{
	pthread_mutex_lock(&registry);
	bool value = stage < 2 && entered[stage];
	pthread_mutex_unlock(&registry);
	return value;
}
static void latch(unsigned stage)
{
	pthread_mutex_lock(&registry);
	/* Only the first visitor is held; independent heap queues can proceed. */
	if (held[stage] && !entered[stage]) {
		entered[stage] = true;
		pthread_cond_broadcast(&changed);
		while (held[stage])
			pthread_cond_wait(&changed, &registry);
	}
	pthread_mutex_unlock(&registry);
}
static struct generation *find(ove_queue_t q)
{
	for (unsigned i = 0; i < next_generation; ++i)
		if (generations[i].state != UNUSED && generations[i].backing == q)
			return &generations[i];
	return NULL;
}

int ove_queue_init(ove_queue_t *out, ove_queue_storage_t *storage, void *buffer, size_t item_size,
		   unsigned capacity)
{
	pthread_mutex_lock(&registry);
	++counts[INIT];
	if (!allowed(1)) {
		++counts[FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_NOT_SUPPORTED;
	}
	if (find(storage)) {
		++counts[DUPLICATE];
		pthread_mutex_unlock(&registry);
		return OVE_ERR_BUSY;
	}
	if (next_generation == 128)
		reject("generation capacity");
	struct generation *g = &generations[next_generation++];
	*g = (struct generation){.backing = storage, .state = INITIALIZING, .heap = creating};
	unsigned fault = failure;
	failure = 0;
	pthread_mutex_unlock(&registry);
	latch(0);
	ove_queue_t local = NULL;
	int rc = fault == 1 ? OVE_ERR_NO_MEMORY
			    : native_queue_init(&local, storage, buffer, item_size, capacity);
	if (rc == OVE_OK) {
		pthread_mutex_lock(&registry);
		++counts[NATIVE_INIT];
		pthread_mutex_unlock(&registry);
		if (fault == 2) {
			native_queue_deinit(local);
			pthread_mutex_lock(&registry);
			++counts[NATIVE_DEINIT];
			pthread_mutex_unlock(&registry);
			rc = OVE_ERR_NO_MEMORY;
		}
	}
	pthread_mutex_lock(&registry);
	g->state = rc == OVE_OK ? LIVE : UNUSED;
	if (rc != OVE_OK)
		g->backing = NULL;
	pthread_mutex_unlock(&registry);
	/* Error output remains unchanged, including after a fully cleaned init. */
	if (rc == OVE_OK)
		*out = local;
	return rc;
}

static struct generation *begin_cleanup(ove_queue_t q, unsigned counter)
{
	pthread_mutex_lock(&registry);
	++counts[counter];
	struct generation *g = find(q);
	if (!allowed(1) || !g || g->state != LIVE || g->active) {
		++counts[FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return NULL;
	}
	g->state = TEARING_DOWN;
	pthread_mutex_unlock(&registry);
	return g;
}
static void native_deinit(struct generation *g)
{
	native_queue_deinit(g->backing);
	pthread_mutex_lock(&registry);
	++counts[NATIVE_DEINIT];
	pthread_mutex_unlock(&registry);
}
static void finish_cleanup(struct generation *g)
{
	pthread_mutex_lock(&registry);
	g->backing = NULL;
	g->state = UNUSED;
	pthread_mutex_unlock(&registry);
}
void ove_queue_deinit(ove_queue_t q)
{
	if (destroying) {
		if (destroying->backing != q)
			reject("nested heap cleanup handle");
		pthread_mutex_lock(&registry);
		++counts[DEINIT];
		pthread_mutex_unlock(&registry);
		native_deinit(destroying);
		/* Clear the pointer before common heap destruction can free it. */
		finish_cleanup(destroying);
		return;
	}
	struct generation *g = begin_cleanup(q, DEINIT);
	if (!g)
		return;
	native_deinit(g);
	latch(1);
	finish_cleanup(g);
}

#ifndef CONFIG_OVE_ZERO_HEAP
int ove_queue_create(ove_queue_t *out, size_t item_size, unsigned capacity)
{
	pthread_mutex_lock(&registry);
	++counts[CREATE];
	bool ok = allowed(1);
	if (!ok)
		++counts[FORBIDDEN];
	pthread_mutex_unlock(&registry);
	if (!ok)
		return OVE_ERR_NOT_SUPPORTED;
	creating = true;
	int rc = native_queue_create(out, item_size, capacity);
	creating = false;
	return rc;
}
void ove_queue_destroy(ove_queue_t q)
{
	struct generation *g = begin_cleanup(q, DESTROY);
	if (!g)
		return;
	latch(1);
	destroying = g;
	native_queue_destroy(q);
	destroying = NULL;
}
#endif

static struct generation *begin_operation(ove_queue_t q, unsigned counter, bool irq)
{
	pthread_mutex_lock(&registry);
	++counts[counter];
	struct generation *g = find(q);
	bool context_ok = irq ? (native_is_in_isr() || (counter == ISR_COUNT && allowed(2)))
			      : allowed(counter == NOTIFY ? 1 : 2);
	if (!context_ok || !g || g->state != LIVE) {
		++counts[FORBIDDEN];
		pthread_mutex_unlock(&registry);
		return NULL;
	}
	++g->active;
	pthread_mutex_unlock(&registry);
	return g;
}
static void end_operation(struct generation *g)
{
	pthread_mutex_lock(&registry);
	if (!g->active)
		reject("operation reservation underflow");
	--g->active;
	pthread_mutex_unlock(&registry);
}
static void task_timeout(uint64_t timeout)
{
	pthread_mutex_lock(&registry);
	counts[TASK_ZERO] += timeout == 0;
	pthread_mutex_unlock(&registry);
}
int ove_queue_send(ove_queue_t q, const void *data, uint64_t timeout)
{
	struct generation *g = begin_operation(q, SEND, false);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	task_timeout(timeout);
	int rc = native_queue_send(q, data, timeout);
	end_operation(g);
	return rc;
}
int ove_queue_receive(ove_queue_t q, void *data, uint64_t timeout)
{
	struct generation *g = begin_operation(q, RECEIVE, false);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	task_timeout(timeout);
	int rc = native_queue_receive(q, data, timeout);
	end_operation(g);
	return rc;
}
int ove_queue_send_from_isr(ove_queue_t q, const void *data)
{
	struct generation *g = begin_operation(q, ISR_SEND, true);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	int rc = native_queue_send_from_isr(q, data);
	end_operation(g);
	return rc;
}
int ove_queue_receive_from_isr(ove_queue_t q, void *data)
{
	struct generation *g = begin_operation(q, ISR_RECEIVE, true);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	int rc = native_queue_receive_from_isr(q, data);
	end_operation(g);
	return rc;
}
size_t ove_queue_count(ove_queue_t q)
{
	struct generation *g = begin_operation(q, COUNT, false);
	if (!g)
		return 0;
	size_t n = native_queue_count(q);
	end_operation(g);
	return n;
}
size_t ove_queue_count_from_isr(ove_queue_t q)
{
	struct generation *g = begin_operation(q, ISR_COUNT, true);
	if (!g)
		return 0;
	size_t n = native_queue_count_from_isr(q);
	end_operation(g);
	return n;
}
size_t ove_queue_capacity(ove_queue_t q)
{
	struct generation *g = begin_operation(q, CAPACITY, false);
	if (!g)
		return 0;
	size_t n = native_queue_capacity(q);
	end_operation(g);
	return n;
}
int ove_queue_set_notify(ove_queue_t q, ove_notify_cb cb, void *data)
{
	struct generation *g = begin_operation(q, NOTIFY, false);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	int rc = native_queue_set_notify(q, cb, data);
	end_operation(g);
	return rc;
}

#ifdef CONFIG_OVE_TIME
int ove_time_get_ns(uint64_t *out)
{
	pthread_mutex_lock(&registry);
	++counts[CLOCK];
	bool ok = allowed(2);
	if (!ok)
		++counts[FORBIDDEN_CLOCK];
	pthread_mutex_unlock(&registry);
	if (!ok)
		return OVE_ERR_NOT_SUPPORTED;
	*out = 1000;
	return OVE_OK;
}
#endif

/* Called after every owner, worker, callback and latch visitor is quiescent.
 * This discards inaccessible bytes; it never reads T or clears a Rust claim. */
unsigned queue_test_cleanup_retained(void)
{
	if (!allowed(1) || held[0] || held[1])
		reject("retirement before context/latch restoration");
	unsigned retired = 0;
	for (unsigned i = 0; i < next_generation; ++i) {
		struct generation *g = &generations[i];
		if (g->state == UNUSED)
			continue;
		if (g->state != LIVE || g->active || !g->backing)
			reject("retirement before operation quiescence");
		g->state = TEARING_DOWN;
#ifndef CONFIG_OVE_ZERO_HEAP
		if (g->heap) {
			destroying = g;
			native_queue_destroy(g->backing);
			destroying = NULL;
		} else
#endif
		{
			native_deinit(g);
			finish_cleanup(g);
		}
		++retired;
	}
	counts[RETIRED] += retired;
	return retired;
}
