/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This sequential model never deletes a pending native target. Retirement
 * offers describe permitted task progress; private retention keeps every
 * token initialized through its final modeled native use. */
struct model_task {
	unsigned generation, pending;
	bool initialized;
};
enum phase { NO_RETIREMENT, LATCH_RECHECK, COMMITTED_TIMEOUT };
enum wake {
	NO_WAKE,
	REGISTER_WAKE,
	TASK_WAKE,
	ISR_TASK_WAKE,
	ISR_WAKE,
	UNRELATED_WAKE,
	RACING_WAKE,
	COMMITTED_WAKE
};
static unsigned failures, controls, generation;
static ove_event_storage_t storage;
static struct model_task token;
static struct {
	ove_event_t event;
	TaskHandle_t copied;
	enum phase phase;
	enum wake wake;
	unsigned mask, task_depth, native_depth, tick, takes;
	unsigned loads, calls, checks, offers, deferrals, retentions, cleanups, last_uses;
	unsigned registration, deregistration, yields, retired;
	unsigned expected_mask;
	unsigned copied_generation;
	bool trial, in_native, irq, retained, injected, registered_now, deferred_signal;
} model;

static void check(bool condition, const char *message)
{
	if (!condition) {
		++failures;
		fprintf(stderr, "failed: %s\n", message);
	}
}

static void reject(void)
{
	fputs("fixture rejected: invalid Event observer operation\n", stderr);
	exit(2);
}

static void dispatch(enum wake how);
static bool trial_registered(void);

static void retirement_offer(void)
{
	if (!model.trial || !model.copied || model.phase == NO_RETIREMENT)
		return;
	if (model.copied != &token || !token.initialized || !trial_registered())
		reject();
	/* A runnable waiter before its first latch check can consume this exact
	 * posted latch. A waiter after its last timeout check can instead finish
	 * that committed result and leave the later latch for the next wait. */
	if (model.phase == LATCH_RECHECK && model.event->signaled != 1)
		reject();
	++model.checks;
	if (model.irq || model.mask) {
		++model.deferrals;
		return;
	}
	++model.offers;
	check(false, "task retirement admitted before final notification use");
	/* Record the failure first. No actual backing retirement occurs here. */
	if (!model.retained) {
		model.retained = true;
		++model.retentions;
	}
}

static void observe_native_argument(TaskHandle_t copied)
{
	if (!model.trial || !copied)
		return;
	if (model.copied || copied != &token || !token.initialized)
		reject();
	model.copied = copied;
	model.copied_generation = token.generation;
	++model.loads;
	retirement_offer();
}

static void observe_registration(void)
{
	if (!model.event || model.trial)
		return;
	bool live = model.event->waiters != NULL;
	if (live && !model.registered_now)
		++model.registration;
	if (!live && model.registered_now)
		++model.deregistration;
	model.registered_now = live;
}

static void after_exchange(const void *address, unsigned previous)
{
	observe_registration();
	if (!model.event || address != &model.event->signaled || model.trial ||
	    model.wake != COMMITTED_WAKE || model.takes < 2 || model.injected)
		return;
	check(previous == 0, "committed timeout observed the empty latch");
	model.injected = true;
	if (model.mask)
		model.deferred_signal = true;
	else
		dispatch(TASK_WAKE);
}

static void model_yield(BaseType_t wake)
{
	(void)wake;
	check(model.mask == model.expected_mask, "yield follows caller mask restoration");
	if (model.trial)
		++model.yields;
}

/* Preserve the actual latch operation. Native arguments are observed at the
 * callback boundary; source/native checks cover private record traversal. */
#define __atomic_exchange_n(pointer, value, order)                               \
	({                                                                       \
		__auto_type observed_pointer = (pointer);                        \
		__auto_type observed_value =                                     \
			__atomic_exchange_n(observed_pointer, (value), (order)); \
		after_exchange(observed_pointer, observed_value);                \
		observed_value;                                                  \
	})
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(wake) model_yield(wake)
#include "ove/trace.h"
static void observe_trace(const char *, const char *, const void *);
#undef OVE_TRACE_MARK_CURRENT
#define OVE_TRACE_MARK_CURRENT(primitive, action, object) \
	observe_trace(#primitive, #action, (object))
#include OVE_SYNC_BACKEND_SOURCE
#undef OVE_TRACE_MARK_CURRENT
#undef __atomic_exchange_n

static struct freertos_event_waiter trial_record;
static bool trial_registered(void)
{
	return model.event->waiters == &trial_record && trial_record.task == &token &&
	       trial_record.next == NULL;
}
static void observe_trace(const char *primitive, const char *action, const void *object)
{
	if (strcmp(primitive, "OVE_TRACE_PRIM_EVENT") || strcmp(action, "OVE_TRACE_ACT_WAIT_EXIT"))
		return;
	check(object == model.event && !model.mask && !model.event->waiters,
	      "automatic record retires before its owning call returns");
	/* Keep an invalid retirement observable without allowing a subsequent
	 * modeled producer to access expired automatic storage. */
	model.event->waiters = NULL;
	observe_registration();
}

void model_enter_critical(void)
{
	check(!model.irq, "task-only critical entry requires task context");
	++model.task_depth;
	model.mask = 1;
	if (model.in_native)
		++model.native_depth;
}

void model_exit_critical(void)
{
	if (!model.task_depth || (model.in_native && !model.native_depth))
		reject();
	if (model.in_native)
		--model.native_depth;
	observe_registration();
	if (!--model.task_depth) {
		model.mask = 0;
		if (model.deferred_signal ||
		    (model.registered_now && model.wake == REGISTER_WAKE && !model.injected)) {
			model.deferred_signal = false;
			model.injected = true;
			dispatch(TASK_WAKE);
		}
	}
}

UBaseType_t model_enter_critical_from_isr(void)
{
	UBaseType_t saved = model.mask;
	model.mask = 1;
	if (model.in_native)
		++model.native_depth;
	return saved;
}

void model_exit_critical_from_isr(UBaseType_t saved)
{
	if (saved > 1 || (model.in_native && !model.native_depth))
		reject();
	if (model.in_native)
		--model.native_depth;
	model.mask = saved;
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	if (!token.initialized)
		reject();
	return &token;
}

TickType_t xTaskGetTickCount(void)
{
	return model.tick++;
}

void vTaskSetTimeOutState(TimeOut_t *state)
{
	/* Existing retirement schedules remain within the first tick cycle. */
	state->xOverflowCount = 0;
	state->xTimeOnEntering = xTaskGetTickCount();
}

static void before_native(TaskHandle_t target)
{
	if (model.in_native || target != &token || !token.initialized ||
	    token.generation != generation)
		reject();
	if (model.trial) {
		observe_native_argument(target);
		if (model.copied != target || model.copied_generation != token.generation)
			reject();
		++model.calls;
		/* This checkpoint is before the inner mask, where a shortened
		 * adapter boundary would again permit task retirement. */
		retirement_offer();
	}
	model.in_native = true;
}

static void native_last_use(TaskHandle_t target)
{
	if (!model.in_native || model.native_depth != 1 || model.mask != 1 || target != &token ||
	    !token.initialized || token.generation != generation ||
	    (model.trial && model.copied_generation != token.generation))
		reject();
	++target->pending;
	if (model.trial)
		++model.last_uses;
}

static void after_native(void)
{
	if (model.native_depth)
		reject();
	model.in_native = false;
	/* The pinned native return tail has no later target dereference. */
	model.copied = NULL;
	model.copied_generation = 0;
	if (model.retained) {
		model.retained = false;
		++model.cleanups;
	}
}

BaseType_t xTaskNotifyGive(TaskHandle_t target)
{
	before_native(target);
	model_enter_critical();
	native_last_use(target);
	model_exit_critical();
	after_native();
	return pdTRUE;
}

void vTaskNotifyGiveFromISR(TaskHandle_t target, BaseType_t *wake)
{
	before_native(target);
	UBaseType_t saved = model_enter_critical_from_isr();
	native_last_use(target);
	*wake = model.phase == NO_RETIREMENT ? pdTRUE : pdFALSE;
	model_exit_critical_from_isr(saved);
	after_native();
}

static void dispatch(enum wake how)
{
	if (how == TASK_WAKE) {
		ove_event_signal(model.event);
	} else {
		bool old_irq = model.irq;
		model.irq = how == ISR_WAKE;
		ove_event_signal_from_isr(model.event);
		model.irq = old_irq;
	}
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	if (clear != pdTRUE || !token.initialized)
		reject();
	++model.takes;
	if (ticks) {
		check(!model.mask && !model.task_depth && !model.native_depth,
		      "blocking wait stays outside protection");
		if (model.wake == UNRELATED_WAKE && model.takes == 2) {
			++token.pending;
		} else if (model.wake != NO_WAKE && model.wake != COMMITTED_WAKE &&
			   !model.injected) {
			model.injected = true;
			dispatch(model.wake == ISR_WAKE	       ? ISR_WAKE
				 : model.wake == ISR_TASK_WAKE ? ISR_TASK_WAKE
							       : TASK_WAKE);
			if (model.wake == RACING_WAKE)
				return 0; /* the timeout result committed before signal */
		}
	}
	uint32_t pending = token.pending;
	token.pending = 0;
	if (ticks && !pending) {
		if (ticks == portMAX_DELAY)
			reject();
		model.tick += ticks;
	}
	return pending;
}

/* Unselected primitive operations must never become accidental controls. */
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{
	(void)s;
	reject();
	return NULL;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s)
{
	return xSemaphoreCreateMutexStatic(s);
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned initial, unsigned maximum,
						 StaticSemaphore_t *s)
{
	(void)initial;
	(void)maximum;
	return xSemaphoreCreateMutexStatic(s);
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{
	(void)s;
	reject();
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
	(void)s;
	(void)ticks;
	reject();
	return pdFALSE;
}
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t ticks)
{
	return xSemaphoreTake(s, ticks);
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
	return xSemaphoreTake(s, 0);
}
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s)
{
	return xSemaphoreTake(s, 0);
}

static void finish(void)
{
	if (model.copied || model.copied_generation || model.retained || model.in_native ||
	    model.native_depth || model.mask || model.task_depth || model.event->waiters)
		reject();
	ove_event_deinit(model.event);
	token.initialized = false;
}

static void start(void)
{
	if (token.initialized || model.copied || model.retained)
		reject();
	memset(&model, 0, sizeof(model));
	token = (struct model_task){.generation = ++generation, .initialized = true};
	if (ove_event_init(&model.event, &storage) || model.event != &storage)
		reject();
}

static void wait_control(enum wake wake, int expected, unsigned takes)
{
	start();
	model.wake = wake;
	int rc = ove_event_wait(model.event, 20000000);
	check(rc == expected && model.takes == takes, "actual wait result and remaining-time path");
	check(model.registration == 1 && model.deregistration == 1 && !model.event->waiters,
	      "actual wait registration and retirement");
	finish();
	++controls;
}

static void ordinary_controls(void)
{
	start();
	ove_event_signal(model.event);
	dispatch(ISR_TASK_WAKE);
	dispatch(ISR_WAKE);
	check(ove_event_wait(model.event, 0) == OVE_OK, "presignal and coalescing");
	check(ove_event_wait(model.event, 0) == OVE_ERR_TIMEOUT, "coalesced signal consumed once");
	finish();
	++controls;
	wait_control(REGISTER_WAKE, OVE_OK, 2);
	wait_control(TASK_WAKE, OVE_OK, 2);
	wait_control(ISR_TASK_WAKE, OVE_OK, 2);
	wait_control(ISR_WAKE, OVE_OK, 2);
	wait_control(NO_WAKE, OVE_ERR_TIMEOUT, 2);
	wait_control(RACING_WAKE, OVE_OK, 2);
	start();
	model.wake = COMMITTED_WAKE;
	check(ove_event_wait(model.event, 20000000) == OVE_ERR_TIMEOUT &&
		      model.event->signaled == 1 && model.takes == 2,
	      "committed timeout retains a later signal");
	check(ove_event_wait(model.event, 0) == OVE_OK, "later latch survives committed timeout");
	finish();
	++controls;
	start();
	token.pending = 1;
	check(ove_event_wait(model.event, 20000000) == OVE_ERR_TIMEOUT && model.takes == 2,
	      "initial stale notification is drained");
	finish();
	++controls;
	wait_control(UNRELATED_WAKE, OVE_OK, 3);
	start();
	model.wake = TASK_WAKE;
	check(ove_event_wait(model.event, 20000000) == OVE_OK,
	      "new generation reuses retired backing");
	finish();
	++controls;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	const char *name = argv[1];
	bool canary = !strcmp(name, "canary");
	const char *selected = canary ? "task_latch" : name;
	if (canary)
		check(false, "explicit early failure canary");
	start();
	bool isr = !strncmp(selected, "isr_", 4);
	bool genuine = !strncmp(selected, "isr_true_", 9);
	bool masked = !strncmp(selected, "isr_masked_", 11);
	bool no_waiter = !strcmp(selected, "no_waiter") || !strcmp(selected, "controls");
	bool timeout = strstr(selected, "_timeout") != NULL;
	if (!no_waiter && strcmp(selected, "task_latch") && strcmp(selected, "task_timeout") &&
	    strcmp(selected, "isr_task_latch") && strcmp(selected, "isr_task_timeout") &&
	    strcmp(selected, "isr_true_latch") && strcmp(selected, "isr_true_timeout") &&
	    strcmp(selected, "isr_masked_latch") && strcmp(selected, "isr_masked_timeout"))
		return 2;
	model.trial = true;
	model.phase = no_waiter ? NO_RETIREMENT : timeout ? COMMITTED_TIMEOUT : LATCH_RECHECK;
	model.irq = genuine;
	if (!no_waiter) {
		trial_record = (struct freertos_event_waiter){.task = &token, .next = NULL};
		model.event->waiters = &trial_record;
	}
	if (masked)
		model_enter_critical();
	model.expected_mask = model.mask;
	dispatch(genuine ? ISR_WAKE : isr ? ISR_TASK_WAKE : TASK_WAKE);
	unsigned returned_mask = model.mask;
	check(returned_mask == model.expected_mask, "caller native mask is restored");
	if (masked)
		model_exit_critical();
	check(!model.copied && !model.retained && !model.native_depth,
	      "last native use precedes private retention cleanup");
	/* Only now can the simulated waiter finish and its owner reuse backing. */
	model.event->waiters = NULL;
	model.retired = !no_waiter;
	unsigned loads = model.loads, calls = model.calls, checks = model.checks;
	unsigned offers = model.offers, deferrals = model.deferrals;
	unsigned retentions = model.retentions, cleanups = model.cleanups;
	unsigned last_uses = model.last_uses, yields = model.yields, retired = model.retired;
	unsigned caller_mask = model.expected_mask;
	model.irq = false;
	finish();
	ordinary_controls();
	printf("%s: failures=%u controls=%u loads=%u calls=%u checks=%u offers=%u "
	       "deferrals=%u retentions=%u cleanups=%u last_uses=%u yields=%u retired=%u "
	       "caller_mask=%u returned_mask=%u\n",
	       name, failures, controls, loads, calls, checks, offers, deferrals, retentions,
	       cleanups, last_uses, yields, retired, caller_mask, returned_mask);
	return failures != 0;
}
