/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "provider.h"
#include "ove/irq.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Host threads schedule initialized observer entries. No FreeRTOS task,
 * scheduler, queue or blocking implementation is linked. */
struct model_task {
	pthread_cond_t changed;
	pthread_t thread;
	ove_event_t event;
	uint64_t timeout;
	unsigned pending, hints;
	int result;
	bool assigned, spawned, active, waiting, done, returned, suspended, aborted, origin;
};
static struct model_task entries[8];
static pthread_mutex_t kernel = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t progress = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static _Thread_local struct model_task *self;
static _Thread_local unsigned depth;
static _Thread_local bool privileged = true, interrupt;
static atomic_uint failures;
static uint64_t ticks;
static unsigned signal_hints, signal_done;

static void reject_at(const char *function, unsigned line)
{
	fprintf(stderr, "fixture rejected: host observer %s:%u self=%td depth=%u\n", function, line,
		self ? self - entries : -1, depth);
	exit(2);
}
#define reject() reject_at(__func__, __LINE__)
void fixture_check(bool ok, const char *message)
{
	if (!ok) {
		atomic_fetch_add_explicit(&failures, 1, memory_order_relaxed);
		printf("FAIL: %s\n", message);
	}
}
static void lock(void)
{
	if (pthread_mutex_lock(&kernel))
		reject();
}
static void unlock(void)
{
	if (pthread_mutex_unlock(&kernel))
		reject();
}
static void initialize(void)
{
	for (unsigned i = 0; i < 8; ++i)
		if (pthread_cond_init(&entries[i].changed, NULL))
			reject();
}
static struct timespec deadline(void)
{
	struct timespec limit;
	if (clock_gettime(CLOCK_REALTIME, &limit))
		reject();
	limit.tv_sec += 2;
	return limit;
}
static void await_at(pthread_cond_t *condition, const struct timespec *limit, const char *caller)
{
	int status = pthread_cond_timedwait(condition, &kernel, limit);
	if (status != 0) {
		/* timedwait returns with this observer's mutex held. */
		fprintf(stderr, "observer clock=%llu\n", (unsigned long long)ticks);
		for (unsigned i = 0; i < 8; ++i)
			fprintf(stderr,
				"entry%u assigned=%u spawned=%u active=%u waiting=%u done=%u "
				"returned=%u pending=%u hints=%u suspended=%u aborted=%u\n",
				i, entries[i].assigned, entries[i].spawned, entries[i].active,
				entries[i].waiting, entries[i].done, entries[i].returned,
				entries[i].pending, entries[i].hints, entries[i].suspended,
				entries[i].aborted);
		reject_at(caller, __LINE__);
	}
}
#define await(condition, limit) await_at(condition, limit, __func__)
static struct model_task *current(void)
{
	if (pthread_once(&once, initialize))
		reject();
	if (!self) {
		lock();
		for (unsigned i = 0; i < 8; ++i)
			if (!entries[i].assigned) {
				self = &entries[i];
				self->assigned = true;
				break;
			}
		unlock();
		if (!self)
			reject();
	}
	return self;
}
void fixture_mode(bool mode)
{
	privileged = mode;
}
BaseType_t model_privileged(void)
{
	return privileged ? pdTRUE : pdFALSE;
}
void model_raise(void)
{
	fixture_check(!interrupt, "task transition stays outside interrupt context");
	privileged = true;
}
void model_lower(void)
{
	fixture_check(!interrupt && privileged, "mode restoration follows a task transition");
	privileged = false;
}
void model_enter_critical(void)
{
	fixture_check(!OVE_MODEL_MPU || privileged || interrupt,
		      "task record access starts with effective native privilege");
	if (!depth++)
		lock();
}
void model_exit_critical(void)
{
	fixture_check(depth != 0, "task exclusion exits an owned scope");
	if (depth && !--depth)
		unlock();
}
UBaseType_t model_enter_critical_from_isr(void)
{
	unsigned previous = depth;
	if (!depth++)
		lock();
	return previous;
}
void model_exit_critical_from_isr(UBaseType_t previous)
{
	fixture_check(depth == previous + 1, "interrupt mask nesting is balanced");
	if (depth && !--depth)
		unlock();
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return current();
}
TickType_t xTaskGetTickCount(void)
{
	lock();
	TickType_t value = (TickType_t)ticks;
	unlock();
	return value;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	lock();
	state->xTimeOnEntering = (TickType_t)ticks;
	state->xOverflowCount = (BaseType_t)((__uint128_t)ticks / ((__uint128_t)portMAX_DELAY + 1));
	unlock();
}
BaseType_t xTaskNotifyGive(TaskHandle_t target)
{
	fixture_check(depth != 0, "native notification completes under outer exclusion");
	bool own = depth == 0;
	if (own)
		lock();
	bool known = false;
	for (unsigned i = 0; i < 8; ++i)
		known |= target == &entries[i] && entries[i].assigned;
	if (!known)
		reject();
	fixture_check(target->active, "only a live wait receives a notification hint");
	++target->pending;
	++target->hints;
	if (!target->suspended)
		pthread_cond_signal(&target->changed);
	pthread_cond_broadcast(&progress);
	if (own)
		unlock();
	return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t target, BaseType_t *higher)
{
	(void)xTaskNotifyGive(target);
	*higher = target == &entries[1] ? pdTRUE : pdFALSE;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t budget)
{
	struct model_task *task = current();
	fixture_check(clear == pdTRUE && !depth && privileged == task->origin,
		      "blocking entry restores caller mode and exclusion");
	lock();
	uint64_t began = ticks;
	struct timespec limit = deadline();
	if (budget) {
		task->waiting = true;
		pthread_cond_broadcast(&progress);
		while ((!task->pending || task->suspended) && !task->aborted &&
		       (budget == portMAX_DELAY || ticks - began < budget))
			await(&task->changed, &limit);
		task->waiting = false;
	}
	unsigned value = task->aborted ? 0 : task->pending;
	task->aborted = false;
	task->pending = 0;
	unlock();
	return value;
}
void *pvPortMalloc(size_t size)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	fixture_check(false, "caller-owned Event makes no allocation attempt");
#endif
	return malloc(size);
}
void vPortFree(void *pointer)
{
	free(pointer);
}
uint32_t ove_sync_context(void)
{
	return OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING | OVE_SYNC_CONTEXT_PRODUCER;
}
bool ove_is_in_isr(void)
{
	return interrupt;
}

#include OVE_EVENT_SOURCE

void model_trace(const char *primitive, const char *action, const void *object)
{
	if (strcmp(primitive, "OVE_TRACE_PRIM_EVENT") ||
	    (strcmp(action, "OVE_TRACE_ACT_WAIT_ENTER") &&
	     strcmp(action, "OVE_TRACE_ACT_WAIT_EXIT")))
		return;
	struct model_task *task = current();
	bool enter = !strcmp(action, "OVE_TRACE_ACT_WAIT_ENTER");
	if (!enter) {
		fixture_check(!depth && privileged == task->origin,
			      "completed call restores its original mode and exclusion");
		/* A bad mode/mask mutant is recorded before repairing only this
		 * observer's owned state, allowing remaining controls to finish. */
		while (depth)
			model_exit_critical();
		privileged = task->origin;
	}
	lock();
	if (enter) {
		task->origin = privileged;
		task->active = true;
		task->done = false;
		task->event = (ove_event_t)object;
	} else {
		ove_event_t evt = (ove_event_t)object;
#if OVE_MODEL_EVENT_LIST
		struct freertos_event_waiter **link = &evt->waiters;
		while (*link) {
			if ((*link)->task == task) {
				fixture_check(false,
					      "automatic record retires before its lifetime ends");
				*link = (*link)->next;
			} else {
				link = &(*link)->next;
			}
		}
#else
		TaskHandle_t slot = __atomic_load_n(&evt->waiter, __ATOMIC_SEQ_CST);
		fixture_check(slot != task, "completed invocation retires its own slot");
		if (slot == task)
			__atomic_store_n(&evt->waiter, (TaskHandle_t)NULL, __ATOMIC_SEQ_CST);
#endif
		task->active = false;
		task->done = true;
	}
	pthread_cond_broadcast(&progress);
	unlock();
}
static unsigned done_mask(void)
{
	unsigned mask = 0;
	for (unsigned i = 0; i < 8; ++i)
		if (entries[i].done)
			mask |= 1u << i;
	return mask;
}
void fixture_prepare_signal(void)
{
	lock();
	signal_hints = 0;
	for (unsigned i = 0; i < 8; ++i)
		if (entries[i].active && !entries[i].suspended)
			signal_hints += entries[i].hints;
	signal_done = done_mask();
	unlock();
}
void fixture_await_signal(void)
{
	lock();
	unsigned hints = 0;
	for (unsigned i = 0; i < 8; ++i)
		if (entries[i].active && !entries[i].suspended)
			hints += entries[i].hints;
	if (hints == signal_hints && done_mask() == signal_done) {
		fixture_check(false, "separated signal supplies a live waiter with a prompt hint");
		/* Preserve the failed observation, then release one initialized
		 * entry while the posted target latch is still available. */
		for (unsigned i = 0; i < 8; ++i)
			if (entries[i].active && entries[i].waiting && !entries[i].suspended) {
				++entries[i].pending;
				pthread_cond_signal(&entries[i].changed);
				break;
			}
	}
	struct timespec limit = deadline();
	while (done_mask() == signal_done)
		await(&progress, &limit);
	unlock();
}
void fixture_ready_count(unsigned count)
{
	lock();
	struct timespec limit = deadline();
	for (;;) {
		unsigned waiting = 0;
		for (unsigned i = 0; i < 8; ++i)
			waiting += entries[i].waiting && !entries[i].pending;
		if (waiting >= count)
			break;
		await(&progress, &limit);
	}
	unlock();
}
static void *worker(void *argument)
{
	self = argument;
	privileged = self->origin;
	int result = ove_event_wait(self->event, self->timeout);
	lock();
	self->result = result;
	self->returned = true;
	pthread_cond_broadcast(&progress);
	unlock();
	return NULL;
}
void fixture_reset(void)
{
	if (pthread_once(&once, initialize) || depth)
		reject();
	lock();
	for (unsigned i = 0; i < 8; ++i) {
		if (entries[i].active || entries[i].spawned)
			reject();
		/* Keep initialized pthread objects intact between quiescent cases. */
		entries[i].assigned = entries[i].waiting = entries[i].done = false;
		entries[i].returned = entries[i].suspended = entries[i].aborted = false;
		entries[i].pending = entries[i].hints = 0;
	}
	ticks = 0;
	self = NULL;
	privileged = true;
	unlock();
}
void fixture_start(unsigned id, ove_event_t evt, uint64_t timeout, bool mode)
{
	if (id >= 4)
		reject();
	lock();
	struct model_task *task = &entries[id];
	if (task->assigned)
		reject();
	task->assigned = task->spawned = true;
	task->event = evt;
	task->timeout = timeout;
	task->origin = mode;
	unlock();
	if (pthread_create(&task->thread, NULL, worker, task))
		reject();
}
void fixture_join(void)
{
	for (unsigned i = 0; i < 8; ++i)
		if (entries[i].spawned) {
			if (pthread_join(entries[i].thread, NULL))
				reject();
			entries[i].spawned = false;
		}
}
void fixture_waiting(unsigned mask)
{
	lock();
	struct timespec limit = deadline();
	for (;;) {
		unsigned ready = 0;
		for (unsigned i = 0; i < 8; ++i)
			if (entries[i].waiting && !entries[i].pending)
				ready |= 1u << i;
		if (((ready | done_mask()) & mask) == mask) {
			fixture_check(
				(ready & mask) == mask,
				"remaining waiters reblock without consuming another occurrence");
			break;
		}
		await(&progress, &limit);
	}
	unlock();
}
void fixture_finished(unsigned mask)
{
	lock();
	struct timespec limit = deadline();
	for (;;) {
		unsigned returned = 0;
		for (unsigned i = 0; i < 8; ++i)
			if (entries[i].returned)
				returned |= 1u << i;
		if ((returned & mask) == mask)
			break;
		await(&progress, &limit);
	}
	unlock();
}
unsigned fixture_finished_mask(void)
{
	lock();
	unsigned result = done_mask();
	unlock();
	return result;
}
int fixture_result(unsigned id)
{
	lock();
	int result = entries[id].result;
	unlock();
	return result;
}
unsigned fixture_hints(unsigned id)
{
	lock();
	unsigned result = entries[id].hints;
	unlock();
	return result;
}
void fixture_hint(unsigned id)
{
	lock();
	++entries[id].pending;
	pthread_cond_signal(&entries[id].changed);
	unlock();
}
void fixture_advance(uint64_t amount)
{
	lock();
	ticks += amount;
	for (unsigned i = 0; i < 8; ++i)
		pthread_cond_broadcast(&entries[i].changed);
	unlock();
}
void fixture_suspend(unsigned id, bool suspended)
{
	lock();
	entries[id].suspended = suspended;
	if (!suspended)
		pthread_cond_signal(&entries[id].changed);
	unlock();
}
void fixture_abort(unsigned id)
{
	lock();
	entries[id].aborted = true;
	pthread_cond_signal(&entries[id].changed);
	unlock();
}
void fixture_signal(ove_event_t evt, bool from_isr, bool nested)
{
	bool before = privileged;
	unsigned previous = depth;
	if (nested) {
		/* Model an already established caller mask; this setup does not
		 * claim an unprivileged raw native critical entry is available. */
		privileged = true;
		model_enter_critical();
		privileged = before;
	}
	if (from_isr) {
		interrupt = true;
		ove_event_signal_from_isr(evt);
		interrupt = false;
	} else {
		ove_event_signal(evt);
	}
	fixture_check(privileged == before && depth == previous + (unsigned)nested,
		      "producer restores its caller mode and nested mask");
	while (depth > previous)
		model_exit_critical();
	privileged = before;
}
unsigned fixture_failures(void)
{
	return atomic_load_explicit(&failures, memory_order_relaxed);
}
void fixture_binding_start(void)
{
	fixture_reset();
	const char *canary = getenv("OVE_EVENT_BINDING_CANARY");
	if (canary && !strcmp(canary, "1"))
		fixture_check(false, "early failure survives later binding controls");
}
int fixture_binding_report(const char *language)
{
	lock();
	unsigned completed = 0;
	for (unsigned i = 0; i < 8; ++i) {
		completed += entries[i].done;
		fixture_check(!entries[i].active,
			      "binding owner outlives every active native wait");
	}
	unlock();
	fixture_check(completed == 2, "both shared binding waits completed");
	printf("event shared %s: heap=%u completed=%u failures=%u\n", language,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       completed, fixture_failures());
	fflush(stdout);
	return fixture_failures() != 0;
}
