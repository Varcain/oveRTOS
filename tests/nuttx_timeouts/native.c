/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include "ove/queue.h"
#include "ove/sync.h"
#include "ove/eventgroup.h"
#include "ove/stream.h"
#include "ove/timer.h"
#include "ove/storage.h"

/* These ordinary byte objects model native entries. No scheduler, watchdog,
 * kernel task or native blocking implementation is linked. Invalid arguments
 * are recorded and returned from; they are never forwarded to a real kernel. */
static unsigned checks, failures, timed, untimed, clocks, posts, depth, live;
static unsigned starts, cancels, queued, callbacks, wait_count;
static uint32_t delays[16];
static clock_t now;
static int wait_status;
static void (*wake)(void);
static ove_eventgroup_t observed_group;
static ove_stream_t observed_stream;
static unsigned release_at;
static int stream_send;
static mutex_t *reacquire_fail;

static void check(int valid, const char *name)
{
	++checks;
	if (!valid) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static uint64_t limit(void)
{
	uint64_t w = (uint64_t)WDOG_MAX_DELAY - 2;
	return w < (uint64_t)INT32_MAX - 1 ? w : (uint64_t)INT32_MAX - 1;
}
static uint32_t budget(uint64_t ns)
{
	uint64_t ticks = ns / NSEC_PER_TICK + (ns % NSEC_PER_TICK != 0);
	return (uint32_t)(ticks > limit() ? limit() : ticks);
}
static void reset(void)
{
	timed = untimed = clocks = posts = wait_count = 0;
	memset(delays, 0, sizeof(delays));
	now = 100;
	wait_status = -ETIMEDOUT;
	wake = NULL;
	reacquire_fail = NULL;
}
static void route(uint64_t ns, const char *name)
{
	check(timed == (ns == OVE_WAIT_FOREVER ? 0u : 1u) &&
		      untimed == (ns == OVE_WAIT_FOREVER ? 1u : 0u),
	      name);
	if (ns != OVE_WAIT_FOREVER)
		check(delays[0] == budget(ns), "finite native argument equals independent budget");
}
clock_t clock_systime_ticks(void)
{
	++clocks;
	return now;
}
irqstate_t enter_critical_section(void)
{
	return depth++;
}
void leave_critical_section(irqstate_t flags)
{
	check(depth == flags + 1, "critical section balanced");
	depth = flags;
}
bool up_interrupt_context(void)
{
	return false;
}
int nxsem_init(sem_t *s, int shared, int32_t value)
{
	(void)shared;
	s->count = value;
	s->initialized = true;
	++live;
	return 0;
}
int nxsem_destroy(sem_t *s)
{
	check(s->initialized, "destroy initialized semaphore");
	s->initialized = false;
	--live;
	return 0;
}
static int observed_wait(sem_t *s)
{
	check(s->initialized, "wait uses initialized model storage");
	++wait_count;
	if (wake)
		wake();
	if (wait_status == 0 && s->count > 0)
		--s->count;
	return wait_status;
}
int nxsem_tickwait_uninterruptible(sem_t *s, uint32_t ticks)
{
	if (timed < 16)
		delays[timed] = ticks;
	++timed;
	return observed_wait(s);
}
int nxsem_wait_uninterruptible(sem_t *s)
{
	++untimed;
	return observed_wait(s);
}
int nxsem_trywait(sem_t *s)
{
	if (s->count <= 0)
		return -EAGAIN;
	--s->count;
	return 0;
}
int nxsem_post(sem_t *s)
{
	++posts;
	++s->count;
	return 0;
}
int nxsem_get_value(sem_t *s, int *value)
{
	*value = s->count;
	return 0;
}
int nxmutex_init(mutex_t *m)
{
	m->initialized = true;
	m->held = 0;
	++live;
	return 0;
}
int nxmutex_destroy(mutex_t *m)
{
	check(m->initialized, "destroy initialized mutex");
	m->initialized = false;
	--live;
	return 0;
}
int nxmutex_lock(mutex_t *m)
{
	if (m == reacquire_fail) {
		reacquire_fail = NULL;
		return -ENOMEM;
	}
	++m->held;
	return 0;
}
int nxmutex_unlock(mutex_t *m)
{
	if (m->held)
		--m->held;
	return 0;
}
int nxmutex_ticklock(mutex_t *m, uint32_t ticks)
{
	(void)m;
	if (timed < 16)
		delays[timed] = ticks;
	++timed;
	return wait_status;
}
int nxrmutex_init(rmutex_t *m)
{
	m->count = 0;
	return nxmutex_init(&m->mutex);
}
int nxrmutex_destroy(rmutex_t *m)
{
	return nxmutex_destroy(&m->mutex);
}
int nxrmutex_lock(rmutex_t *m)
{
	return nxmutex_lock(&m->mutex);
}
int nxrmutex_unlock(rmutex_t *m)
{
	return nxmutex_unlock(&m->mutex);
}
int nxrmutex_ticklock(rmutex_t *m, uint32_t ticks)
{
	return nxmutex_ticklock(&m->mutex, ticks);
}
int wd_start(struct wdog_s *w, clock_t ticks, wdentry_t entry, wdparm_t arg)
{
	++starts;
	w->delay = ticks;
	w->handler = entry;
	w->arg = arg;
	w->active = 1;
	return 0;
}
int wd_cancel(struct wdog_s *w)
{
	++cancels;
	w->active = 0;
	return 0;
}
int work_queue(int id, struct work_s *w, void (*handler)(void *), void *arg, clock_t delay)
{
	(void)id;
	(void)delay;
	++queued;
	w->pending = 1;
	handler(arg);
	w->pending = 0;
	return 0;
}
int work_cancel_sync(int id, struct work_s *w)
{
	(void)id;
	w->pending = 0;
	return 0;
}
static void timer_callback(ove_timer_t t, void *data)
{
	(void)t;
	(void)data;
	++callbacks;
}
static void group_wake(void)
{
	++now;
	if (wait_count >= release_at)
		observed_group->bits |= 1;
	else
		observed_group->bits |= 2; /* An unrelated bit is only a hint. */
}
static void stream_wake(void)
{
	++now;
	if (wait_count >= release_at) {
		if (stream_send)
			ove_ring_clear(&observed_stream->ring);
		else {
			const unsigned char bytes[] = {4, 5};
			ove_ring_write(&observed_stream->ring, bytes, 2);
		}
	}
}
static void queue_cases(void)
{
	ove_queue_storage_t storage;
	unsigned buffer[2], in = 123, out = 0;
	ove_queue_t q = NULL;
	check(ove_queue_init(&q, &storage, buffer, sizeof(in), 2) == OVE_OK, "queue setup");
	const uint64_t values[] = {0,
				   1,
				   NSEC_PER_TICK,
				   NSEC_PER_TICK + 1,
				   (limit() + 1) * NSEC_PER_TICK,
				   UINT64_MAX - 1,
				   OVE_WAIT_FOREVER};
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		reset();
		uint64_t ns = values[i];
		wait_status = -ETIMEDOUT;
		check(ove_queue_send(q, &in, ns) ==
			      (ns == 0 ? OVE_ERR_QUEUE_FULL : OVE_ERR_TIMEOUT),
		      "queue send preserves unavailable result");
		route(ns, "queue send selects finite or explicit forever");
		reset();
		check(ove_queue_receive(q, &out, ns) ==
			      (ns == 0 ? OVE_ERR_QUEUE_EMPTY : OVE_ERR_TIMEOUT),
		      "queue receive preserves unavailable result");
		route(ns, "queue receive selects finite or explicit forever");
	}
	reset();
	wait_status = 0;
	check(ove_queue_send(q, &in, UINT64_MAX - 1) == OVE_OK && ove_queue_count(q) == 1,
	      "finite successful queue copy/count");
	route(UINT64_MAX - 1, "successful send still uses bounded finite entry");
	reset();
	wait_status = 0;
	check(ove_queue_receive(q, &out, UINT64_MAX - 1) == OVE_OK && out == in &&
		      ove_queue_count(q) == 0,
	      "finite successful queue receive/count");
	route(UINT64_MAX - 1, "successful receive still uses bounded finite entry");
	ove_queue_deinit(q);
#ifndef CONFIG_OVE_ZERO_HEAP
	check(ove_queue_create(&q, sizeof(in), 2) == OVE_OK,
	      "actual common heap queue constructor");
	ove_queue_destroy(q);
#endif
}
static void sync_cases(void)
{
	ove_mutex_storage_t ms, rs;
	ove_sem_storage_t ss;
	ove_event_storage_t es;
	ove_condvar_storage_t cs;
	ove_mutex_t m, r;
	ove_sem_t s;
	ove_event_t e;
	ove_condvar_t cv;
	check(ove_mutex_init(&m, &ms) == OVE_OK, "mutex setup");
	check(ove_recursive_mutex_init(&r, &rs) == OVE_OK, "recursive setup");
	check(ove_sem_init(&s, &ss, 0, 2) == OVE_OK, "semaphore setup");
	check(ove_event_init(&e, &es) == OVE_OK, "event setup");
	check(ove_condvar_init(&cv, &cs) == OVE_OK, "condition variable setup");
	const uint64_t values[] = {0,
				   1,
				   NSEC_PER_TICK,
				   NSEC_PER_TICK + 1,
				   (limit() + 1) * NSEC_PER_TICK,
				   UINT64_MAX - 1,
				   OVE_WAIT_FOREVER};
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
		uint64_t ns = values[i];
		reset();
		check(ove_mutex_lock(m, ns) == (ns == OVE_WAIT_FOREVER ? OVE_OK : OVE_ERR_TIMEOUT),
		      "mutex status preserved");
		if (ns == OVE_WAIT_FOREVER)
			check(timed == 0 && m->mtx.held == 1,
			      "forever mutex uses untimed native lock");
		else
			route(ns, "mutex finite route");
		ove_mutex_unlock(m);
		reset();
		check(ove_recursive_mutex_lock(r, ns) ==
			      (ns == OVE_WAIT_FOREVER ? OVE_OK : OVE_ERR_TIMEOUT),
		      "recursive mutex status preserved");
		if (ns == OVE_WAIT_FOREVER)
			check(timed == 0 && r->rmtx.mutex.held == 1,
			      "forever recursive uses untimed lock");
		else
			route(ns, "recursive finite route");
		ove_recursive_mutex_unlock(r);
		reset();
		check(ove_sem_take(s, ns) == OVE_ERR_TIMEOUT, "semaphore timeout preserved");
		route(ns, "semaphore finite/forever route");
		reset();
		check(ove_event_wait(e, ns) == OVE_ERR_TIMEOUT, "event timeout preserved");
		route(ns, "event finite/forever route");
		for (unsigned checked = 0; checked < 2; ++checked) {
			reset();
			m->mtx.held = 1;
			bool held = false;
			int rc = checked ? ove_condvar_wait_checked(cv, m, ns, &held)
					 : ove_condvar_wait(cv, m, ns);
			check(rc == (ns == OVE_WAIT_FOREVER ? OVE_ERR_BUSY : OVE_ERR_TIMEOUT),
			      "condition wait result remains clean only for finite timeout");
			check(m->mtx.held == 1 && (!checked || held) && cv->nwaiters == 0,
			      "condition wait reacquires and retires with truthful ownership");
			route(ns, "condition wait finite/forever route");
			ove_mutex_unlock(m);
		}
	}
	reset();
	m->mtx.held = 1;
	reacquire_fail = &m->mtx;
	bool held = true;
	check(ove_condvar_wait_checked(cv, m, UINT64_MAX - 1, &held) == OVE_ERR_NO_MEMORY &&
		      !held && cv->nwaiters == 0,
	      "failed reacquisition never claims clean timeout or held mutex");
	ove_condvar_deinit(cv);
	ove_event_deinit(e);
	ove_sem_deinit(s);
	ove_recursive_mutex_deinit(r);
	ove_mutex_deinit(m);
#ifndef CONFIG_OVE_ZERO_HEAP
	check(ove_mutex_create(&m) == OVE_OK, "common heap mutex constructor");
	ove_mutex_destroy(m);
	check(ove_sem_create(&s, 0, 2) == OVE_OK, "common heap semaphore constructor");
	ove_sem_destroy(s);
	check(ove_condvar_create(&cv) == OVE_OK, "common heap condition constructor");
	ove_condvar_destroy(cv);
#endif
}
static void group_cases(void)
{
	ove_eventgroup_storage_t storage;
	ove_eventgroup_t g;
	ove_eventbits_t result = 0;
	check(ove_eventgroup_init(&g, &storage) == OVE_OK, "group setup");
	observed_group = g;
	const uint64_t values[] = {NSEC_PER_TICK, UINT64_MAX - 1, OVE_WAIT_FOREVER};
	for (unsigned i = 0; i < 3; ++i) {
		reset();
		g->bits = 0;
		wake = group_wake;
		release_at = 1;
		wait_status = 0;
		check(ove_eventgroup_wait_bits(g, 1, OVE_EG_CLEAR_ON_EXIT, values[i], &result) ==
				      OVE_OK &&
			      result == 1 && g->bits == 0,
		      "group finite prompt wake and clear");
		route(values[i], "group prompt wake finite/forever route");
		check(g->nwaiters == 0, "group waiter retired");
	}
	for (unsigned wrap = 0; wrap < 2; ++wrap) {
		reset();
		now = wrap ? (clock_t)(CLOCK_MAX - 1) : 100;
		g->bits = 0;
		wake = group_wake;
		release_at = 8;
		wait_status = 0;
		check(ove_eventgroup_wait_bits(g, 1, 0, 4 * NSEC_PER_TICK, &result) ==
				      OVE_ERR_TIMEOUT &&
			      result == 2,
		      "irrelevant group hints preserve deadline and snapshot across wrap");
		check(timed == 4 && delays[0] == 4 && delays[1] == 3 && delays[2] == 2 &&
			      delays[3] == 1 && g->nwaiters == 0,
		      "group consumes one decreasing budget");
	}
	reset();
	g->bits = 0;
	check(ove_eventgroup_wait_bits(g, 1, 0, 0, &result) == OVE_ERR_TIMEOUT && !timed &&
		      !untimed,
	      "unmatched group zero poll");
	reset();
	g->bits = 3;
	check(ove_eventgroup_wait_bits(g, 1, OVE_EG_CLEAR_ON_EXIT, 0, &result) == OVE_OK &&
		      result == 3 && g->bits == 2,
	      "matched poll preserves full snapshot and clears only requested bit");
	reset();
	check(ove_eventgroup_wait_bits(g, 0, 0, UINT64_MAX - 1, &result) == OVE_ERR_INVALID_PARAM &&
		      !clocks && !timed,
	      "invalid group wait rejects before clock/native effects");
	ove_eventgroup_deinit(g);
#ifndef CONFIG_OVE_ZERO_HEAP
	check(ove_eventgroup_create(&g) == OVE_OK, "common heap group constructor");
	ove_eventgroup_destroy(g);
#endif
}
static void stream_cases(void)
{
	ove_stream_storage_t storage;
	unsigned char buffer[2], input[] = {1, 2, 3, 4}, out[4] = {0};
	ove_stream_t s;
	size_t count = 99;
	check(ove_stream_init(&s, &storage, buffer, 2, 2) == OVE_OK, "stream setup");
	observed_stream = s;
	for (unsigned send = 0; send < 2; ++send) {
		stream_send = send;
		const uint64_t values[] = {NSEC_PER_TICK, UINT64_MAX - 1, OVE_WAIT_FOREVER};
		for (unsigned i = 0; i < 3; ++i) {
			reset();
			ove_ring_clear(&s->ring);
			if (send)
				ove_ring_write(&s->ring, input, 2);
			wake = stream_wake;
			release_at = 1;
			wait_status = 0;
			int rc = send ? ove_stream_send(s, input, 1, values[i], &count)
				      : ove_stream_receive(s, out, 2, values[i], &count);
			check(rc == OVE_OK && count == (send ? 1u : 2u),
			      "stream prompt wake preserves byte count");
			route(values[i], "stream huge finite is never promoted to forever");
		}
		for (unsigned wrap = 0; wrap < 2; ++wrap) {
			reset();
			now = wrap ? (clock_t)(CLOCK_MAX - 1) : 100;
			ove_ring_clear(&s->ring);
			if (!send)
				ove_ring_write(&s->ring, input, 1);
			wake = stream_wake;
			release_at = 8;
			wait_status = 0;
			int rc = send ? ove_stream_send(s, input, 4, 4 * NSEC_PER_TICK, &count)
				      : ove_stream_receive(s, out, 2, 4 * NSEC_PER_TICK, &count);
			check(rc == OVE_OK && count == (send ? 2u : 0u),
			      "stream timeout preserves short-count and trigger contract across wrap");
			check(timed == 4 && delays[0] == 4 && delays[1] == 3 && delays[2] == 2 &&
				      delays[3] == 1,
			      "stream hints consume one decreasing budget");
		}
		reset();
		count = 99;
		int rc = send ? ove_stream_send(s, NULL, 0, UINT64_MAX - 1, &count)
			      : ove_stream_receive(s, NULL, 0, UINT64_MAX - 1, &count);
		check(rc == OVE_OK && count == 0 && !clocks && !timed && !untimed,
		      "zero-length stream avoids clock and native effects");
	}
	ove_stream_deinit(s);
#ifndef CONFIG_OVE_ZERO_HEAP
	check(ove_stream_create(&s, 2, 2) == OVE_OK, "common heap stream constructor");
	ove_stream_destroy(s);
#endif
}
static void timer_cases(void)
{
	ove_timer_storage_t storage, original, sentinel;
	ove_timer_t t = &sentinel;
	const uint64_t invalid[] = {UINT64_MAX, UINT64_MAX - 1, UINT64_MAX - (NSEC_PER_TICK - 2)};
	for (unsigned i = 0; i < 3; ++i) {
		memset(&storage, 0xa5, sizeof(storage));
		memcpy(&original, &storage, sizeof(storage));
		t = &sentinel;
		unsigned before = starts + cancels + queued;
		check(ove_timer_init_ns(&t, &storage, timer_callback, NULL, invalid[i], 0) ==
			      OVE_ERR_INVALID_PARAM,
		      "oversized timer construction rejects");
		check(t == &sentinel && !memcmp(&storage, &original, sizeof(storage)) &&
			      starts + cancels + queued == before,
		      "rejected timer leaves output/backing/native effects untouched");
	}
	uint64_t max = (uint64_t)WDOG_MAX_DELAY - 1;
	if (max >= UINT32_MAX)
		max = UINT32_MAX - 1;
	const uint64_t valid[] = {0, 1, NSEC_PER_TICK - 1, NSEC_PER_TICK, max * NSEC_PER_TICK};
	for (unsigned shot = 0; shot < 2; ++shot) {
		for (unsigned i = 0; i < 5; ++i) {
			check(ove_timer_init_ns(&t, &storage, timer_callback, NULL, valid[i],
						shot) == OVE_OK,
			      "supported exact timer period initializes");
			uint64_t expected =
				valid[i] / NSEC_PER_TICK + (valid[i] % NSEC_PER_TICK != 0);
			if (!expected)
				expected = 1;
			check(ove_timer_start(t) == OVE_OK && t->wdog.delay == expected,
			      "timer arms exact accepted ticks");
			unsigned before = starts;
			unsigned before_callbacks = callbacks;
			t->wdog.handler(t->wdog.arg);
			check(starts == before + (shot ? 0 : 1) &&
				      callbacks == before_callbacks + 1 &&
				      t->wdog.delay == expected,
			      "periodic rearm keeps exact period and oneshot does not rearm");
			for (unsigned j = 0; j < 3; ++j) {
				memcpy(&original, t, sizeof(original));
				unsigned effects = starts + cancels + queued;
				check(ove_timer_set_period_ns(t, invalid[j]) ==
					      OVE_ERR_INVALID_PARAM,
				      "oversized update rejects");
				check(!memcmp(t, &original, sizeof(original)) &&
					      starts + cancels + queued == effects,
				      "refused timer update preserves period and active schedule");
			}
			ove_timer_deinit(t);
		}
	}
	check(ove_timer_init_ns(&t, &storage, timer_callback, NULL, (max + 1) * NSEC_PER_TICK, 0) ==
		      OVE_ERR_INVALID_PARAM,
	      "first unsupported exact timer boundary rejects");
#ifndef CONFIG_OVE_ZERO_HEAP
	t = &sentinel;
	int rc = ove_timer_create_ns(&t, timer_callback, NULL, UINT64_MAX - 1, 0);
	check(rc == OVE_ERR_INVALID_PARAM && t == &sentinel,
	      "common heap failed timer construction preserves handle and frees backing");
	/* A defective provider may publish a valid unexpected owner. Retire
	 * that owner before continuing the independent construction control. */
	if (rc == OVE_OK)
		ove_timer_destroy(t);
	rc = ove_timer_create_ns(&t, timer_callback, NULL, NSEC_PER_TICK, 0);
	check(rc == OVE_OK, "common heap timer constructor");
	if (rc == OVE_OK)
		ove_timer_destroy(t);
#endif
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(0, "early semantic failure survives later controls");
	queue_cases();
	sync_cases();
	group_cases();
	stream_cases();
	timer_cases();
	check(depth == 0 && live == 0, "all inert resource lifecycles retire");
	printf("nuttx timeout native observers: clock=%u tick=%u heap=%u checks=%u failures=%u\n",
	       OVE_CLOCK_BITS, OVE_TICK_NS,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       checks, failures);
	return failures != 0;
}
