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
	SET,
	CLEAR,
	GET,
	WAIT,
	ISR_SET,
	ISR_CLEAR,
	NOTIFY,
	CLOCK,
	FORBIDDEN_CLOCK,
	RETIRED,
	COUNTERS
};
enum { UNUSED, INITIALIZING, LIVE, TEARING_DOWN };
struct generation {
	ove_eventgroup_t backing;
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

int native_eventgroup_init(ove_eventgroup_t *, ove_eventgroup_storage_t *);
void native_eventgroup_deinit(ove_eventgroup_t);
ove_eventbits_t native_eventgroup_set_bits(ove_eventgroup_t, ove_eventbits_t);
ove_eventbits_t native_eventgroup_clear_bits(ove_eventgroup_t, ove_eventbits_t);
ove_eventbits_t native_eventgroup_get_bits(ove_eventgroup_t);
int native_eventgroup_wait_bits(ove_eventgroup_t, ove_eventbits_t, uint32_t, uint64_t,
				ove_eventbits_t *);
int native_eventgroup_set_notify(ove_eventgroup_t, ove_notify_cb, void *);
#ifndef CONFIG_OVE_ZERO_HEAP
int native_eventgroup_create(ove_eventgroup_t *);
void native_eventgroup_destroy(ove_eventgroup_t);
#endif
uint32_t native_sync_context(void);
bool native_is_in_isr(void);
uint64_t eventgroup_test_actual_depth(void);
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
void eventgroup_test_context(uint32_t flags)
{
	context_override = flags;
}
void eventgroup_test_interrupt(bool value)
{
	if (value)
		posix_irq_enter_isr();
	else
		posix_irq_leave_isr();
}

/* Independent entry admission uses metadata or actual depth, not the answer
 * computed by the product query. No physical signal handler calls a eventgroup. */
static uint32_t admitted(void)
{
	if (native_is_in_isr())
		return 0;
	if (context_override != UINT32_MAX)
		return context_override;
	return eventgroup_test_actual_depth() ? 4u : 7u;
}
static bool allowed(unsigned bit)
{
	return (admitted() & bit) != 0;
}

unsigned eventgroup_test_count(unsigned index)
{
	pthread_mutex_lock(&registry);
	unsigned value = index < COUNTERS ? counts[index] : UINT32_MAX;
	pthread_mutex_unlock(&registry);
	return value;
}
unsigned eventgroup_test_live(void)
{
	pthread_mutex_lock(&registry);
	unsigned count = 0;
	for (unsigned i = 0; i < next_generation; ++i)
		count += generations[i].state != UNUSED;
	pthread_mutex_unlock(&registry);
	return count;
}
void eventgroup_test_failure(unsigned value)
{
	pthread_mutex_lock(&registry);
	failure = value;
	pthread_mutex_unlock(&registry);
}
void eventgroup_test_gate(unsigned stage, bool value)
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
bool eventgroup_test_entered(unsigned stage)
{
	pthread_mutex_lock(&registry);
	bool value = stage < 2 && entered[stage];
	pthread_mutex_unlock(&registry);
	return value;
}
static void latch(unsigned stage)
{
	pthread_mutex_lock(&registry);
	/* Only the first visitor is held; independent heap eventgroups can proceed. */
	if (held[stage] && !entered[stage]) {
		entered[stage] = true;
		pthread_cond_broadcast(&changed);
		while (held[stage])
			pthread_cond_wait(&changed, &registry);
	}
	pthread_mutex_unlock(&registry);
}
static struct generation *find(ove_eventgroup_t q)
{
	for (unsigned i = 0; i < next_generation; ++i)
		if (generations[i].state != UNUSED && generations[i].backing == q)
			return &generations[i];
	return NULL;
}

int ove_eventgroup_init(ove_eventgroup_t *out, ove_eventgroup_storage_t *storage)
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
	ove_eventgroup_t local = NULL;
	int rc = fault == 3   ? -1234
		 : fault == 1 ? OVE_ERR_NO_MEMORY
			      : native_eventgroup_init(&local, storage);
	if (rc == OVE_OK) {
		pthread_mutex_lock(&registry);
		++counts[NATIVE_INIT];
		pthread_mutex_unlock(&registry);
		if (fault == 2) {
			native_eventgroup_deinit(local);
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

static struct generation *begin_cleanup(ove_eventgroup_t q, unsigned counter)
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
	native_eventgroup_deinit(g->backing);
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
void ove_eventgroup_deinit(ove_eventgroup_t q)
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
int ove_eventgroup_create(ove_eventgroup_t *out)
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
	int rc = native_eventgroup_create(out);
	creating = false;
	return rc;
}
void ove_eventgroup_destroy(ove_eventgroup_t q)
{
	struct generation *g = begin_cleanup(q, DESTROY);
	if (!g)
		return;
	latch(1);
	destroying = g;
	native_eventgroup_destroy(q);
	destroying = NULL;
}
#endif

static struct generation *begin_operation(ove_eventgroup_t q, unsigned counter, bool irq)
{
	pthread_mutex_lock(&registry);
	++counts[counter];
	struct generation *g = find(q);
	bool context_ok = irq ? native_is_in_isr() : allowed(counter == NOTIFY ? 1 : 2);
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
/* Deliberately narrower configured mask and distinct IRQ return values are
 * routing observers on ordinary host threads, not physical interrupt proof. */
#define TEST_MASK UINT32_C(0x00ffffff)
static _Thread_local bool forced_wait;
static _Thread_local int wait_result, irq_result;
static uint64_t last_timeout;
ove_eventbits_t ove_eventgroup_valid_mask(void)
{
	return TEST_MASK;
}
void eventgroup_test_wait_result(int result)
{
	forced_wait = true;
	wait_result = result;
}
void eventgroup_test_irq_result(int result)
{
	irq_result = result;
}
uint64_t eventgroup_test_timeout(void)
{
	pthread_mutex_lock(&registry);
	uint64_t result = last_timeout;
	pthread_mutex_unlock(&registry);
	return result;
}
ove_eventbits_t ove_eventgroup_set_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct generation *g = begin_operation(eg, SET, false);
	if (!g)
		return 0;
	ove_eventbits_t result = bits & ~TEST_MASK ? 0 : native_eventgroup_set_bits(eg, bits);
	end_operation(g);
	return result;
}
ove_eventbits_t ove_eventgroup_clear_bits(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct generation *g = begin_operation(eg, CLEAR, false);
	if (!g)
		return 0;
	ove_eventbits_t result = bits & ~TEST_MASK ? 0 : native_eventgroup_clear_bits(eg, bits);
	end_operation(g);
	return result;
}
ove_eventbits_t ove_eventgroup_get_bits(ove_eventgroup_t eg)
{
	struct generation *g = begin_operation(eg, GET, false);
	if (!g)
		return 0;
	ove_eventbits_t result = native_eventgroup_get_bits(eg);
	end_operation(g);
	return result;
}
int ove_eventgroup_wait_bits(ove_eventgroup_t eg, ove_eventbits_t bits, uint32_t flags,
			     uint64_t timeout, ove_eventbits_t *out)
{
	struct generation *g = begin_operation(eg, WAIT, false);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	pthread_mutex_lock(&registry);
	last_timeout = timeout;
	pthread_mutex_unlock(&registry);
	int result;
	if (!bits || (bits & ~TEST_MASK) || (flags & ~3u))
		result = OVE_ERR_INVALID_PARAM;
	else if (forced_wait) {
		forced_wait = false;
		result = wait_result;
		if (result == OVE_OK)
			*out = 0xa5;
	} else
		result = native_eventgroup_wait_bits(eg, bits, flags, timeout, out);
	end_operation(g);
	return result;
}
ove_eventbits_t ove_eventgroup_set_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct generation *g = begin_operation(eg, ISR_SET, true);
	if (!g)
		return 0;
	ove_eventbits_t result = bits & ~TEST_MASK ? 0 : bits | 0x80000000u;
	end_operation(g);
	return result;
}
int ove_eventgroup_clear_bits_from_isr(ove_eventgroup_t eg, ove_eventbits_t bits)
{
	struct generation *g = begin_operation(eg, ISR_CLEAR, true);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	int result = bits & ~TEST_MASK ? OVE_ERR_INVALID_PARAM : irq_result;
	end_operation(g);
	return result;
}
int ove_eventgroup_set_notify(ove_eventgroup_t eg, ove_notify_cb cb, void *data)
{
	struct generation *g = begin_operation(eg, NOTIFY, false);
	if (!g)
		return OVE_ERR_NOT_SUPPORTED;
	int result = native_eventgroup_set_notify(eg, cb, data);
	end_operation(g);
	return result;
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
 * This retires native objects without reopening any Rust claim. */
unsigned eventgroup_test_cleanup_retained(void)
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
			native_eventgroup_destroy(g->backing);
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
