/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sequential initialized observers: no native task, scheduler or blocking
 * implementation is linked. Absolute model time is an independent oracle. */
struct model_task {
	bool initialized;
	unsigned pending;
};
struct step {
	__uint128_t elapsed;
	uint32_t hint;
	bool signal;
};
static struct model_task token = {.initialized = true};
#ifdef CONFIG_OVE_ZERO_HEAP
static ove_event_storage_t backing;
#endif
static unsigned checks, failures, cases, allocations, frees;
static const char *case_name;
static struct {
	ove_event_t event;
	__uint128_t clock, initial, observed, advance_register;
	uint64_t ns;
	struct step steps[4];
	unsigned count, takes, scripted, drains, registered, retired, depth, snapshots;
	unsigned advance_sample, epoch_bias;
	bool observing, sampled, register_signal, committed_signal, injected, prior_abort;
	bool registered_now, deferred_signal;
} model;
static void observe_records(void);
static bool current_record(void);

static void check(bool condition, const char *message)
{
	++checks;
	if (!condition) {
		++failures;
		printf("FAIL: %s: %s\n", case_name ? case_name : "setup", message);
	}
}
static void reject(void)
{
	fputs("fixture rejected: invalid initialized observer state\n", stderr);
	exit(2);
}
static __uint128_t modulus(void)
{
	return (__uint128_t)portMAX_DELAY + 1;
}
static uint64_t budget(uint64_t ns)
{
	if (ns == UINT64_MAX)
		return portMAX_DELAY;
	__uint128_t ticks = ((__uint128_t)ns * 1000 + 999999999) / 1000000000;
	uint64_t limit = (uint64_t)portMAX_DELAY - 1;
	return ticks > limit ? limit : (uint64_t)ticks;
}
static void observe(void)
{
	if (model.sampled)
		model.clock += model.advance_sample;
	else {
		model.initial = model.clock;
		model.sampled = true;
	}
	model.observed = model.clock;
}
void model_enter_critical(void)
{
	++model.depth;
}
void model_exit_critical(void)
{
	if (!model.depth)
		reject();
	observe_records();
	--model.depth;
	if (!model.depth && model.deferred_signal) {
		model.deferred_signal = false;
		ove_event_signal(model.event);
	}
}
UBaseType_t model_enter_critical_from_isr(void)
{
	unsigned previous = model.depth;
	model_enter_critical();
	return previous;
}
void model_exit_critical_from_isr(UBaseType_t previous)
{
	model_exit_critical();
	if (model.depth != previous)
		reject();
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	if (!token.initialized)
		reject();
	model.clock += model.advance_register;
	if (model.register_signal)
		ove_event_signal(model.event);
	return &token;
}
TickType_t xTaskGetTickCount(void)
{
	observe();
	return (TickType_t)model.clock;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	observe();
	__uint128_t count = model.epoch_bias + model.clock / modulus();
	if (count > INT32_MAX)
		reject();
	state->xOverflowCount = (BaseType_t)count;
	state->xTimeOnEntering = (TickType_t)model.clock;
	++model.snapshots;
}
/* Faithful stock check is available for isolated substitution mutations.
 * Its optional task marker is a model flag, never a private native TCB. */
BaseType_t xTaskCheckForTimeOut(TimeOut_t *state, TickType_t *remaining)
{
	observe();
#if INCLUDE_xTaskAbortDelay == 1
	if (model.prior_abort) {
		model.prior_abort = false;
		return pdTRUE;
	}
#endif
#if INCLUDE_vTaskSuspend == 1
	if (*remaining == portMAX_DELAY)
		return pdFALSE;
#endif
	TickType_t now = (TickType_t)model.clock;
	__uint128_t count = model.epoch_bias + model.clock / modulus();
	if (count > INT32_MAX)
		reject();
	TickType_t elapsed = (TickType_t)(now - state->xTimeOnEntering);
	if (((BaseType_t)count != state->xOverflowCount && now >= state->xTimeOnEntering) ||
	    elapsed >= *remaining) {
		*remaining = 0;
		return pdTRUE;
	}
	*remaining -= elapsed;
	state->xTimeOnEntering = now;
	state->xOverflowCount = (BaseType_t)count;
	return pdFALSE;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	if (task != &token || !token.initialized || !model.depth)
		reject();
	++token.pending;
	return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher)
{
	*higher = pdFALSE;
	(void)xTaskNotifyGive(task);
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	if (clear != pdTRUE || !token.initialized || model.depth)
		reject();
	if (!ticks) {
		check(model.event->waiters == NULL, "stale hint drained before registration");
		++model.drains;
		unsigned value = token.pending;
		token.pending = 0;
		return value;
	}
	check(current_record(), "positive wait has an initialized registered token");
	if (model.ns == UINT64_MAX) {
		check(ticks == portMAX_DELAY, "encoded forever preserves native argument");
	} else {
		__uint128_t elapsed = model.observed - model.initial;
		uint64_t initial = budget(model.ns);
		uint64_t want = elapsed < initial ? initial - (uint64_t)elapsed : 0;
		check(want != 0 && ticks == want,
		      "native budget equals independent elapsed oracle");
	}
	++model.takes;
	if (token.pending) {
		unsigned value = token.pending;
		token.pending = 0;
		return value;
	}
#if INCLUDE_xTaskAbortDelay == 1
	/* Actual notification blocking resets a prior operation's abort marker. */
	model.prior_abort = false;
#endif
	unsigned index = model.scripted++;
	if (index >= model.count) {
		check(false, "exhausted logical schedule does not start another positive wait");
		model.clock += ticks;
		return 0;
	}
	struct step step = model.steps[index];
	model.clock += step.elapsed;
	if (step.signal)
		ove_event_signal(model.event);
	if (step.hint)
		token.pending = 0;
	return step.hint;
}
static void exchanged(const void *address, unsigned value)
{
	observe_records();
	if (model.observing && address == &model.event->signaled && !value &&
	    model.committed_signal && model.takes && !model.injected) {
		model.injected = true;
		/* An eligible producer cannot run inside the new protection. Offer
		 * its signal at the first unmasked boundary after retirement. */
		if (model.depth)
			model.deferred_signal = true;
		else
			ove_event_signal(model.event);
	}
}
void *pvPortMalloc(size_t size)
{
	++allocations;
#ifndef CONFIG_OVE_ZERO_HEAP
	return malloc(size);
#else
	(void)size;
	reject();
	return NULL;
#endif
}
void vPortFree(void *pointer)
{
	++frees;
#ifndef CONFIG_OVE_ZERO_HEAP
	free(pointer);
#else
	(void)pointer;
	reject();
#endif
}

#define __atomic_exchange_n(address, value, order)                               \
	({                                                                       \
		__auto_type result = __atomic_exchange_n(address, value, order); \
		exchanged(address, result);                                      \
		result;                                                          \
	})
#include OVE_EVENT_SOURCE
#undef __atomic_exchange_n

static bool current_record(void)
{
	return model.event->waiters != NULL && model.event->waiters->task == &token &&
	       model.event->waiters->next == NULL;
}
static void observe_records(void)
{
	if (!model.observing)
		return;
	bool live = model.event->waiters != NULL;
	if (live && !model.registered_now)
		++model.registered;
	if (!live && model.registered_now)
		++model.retired;
	model.registered_now = live;
}
void model_trace(const char *primitive, const char *action, const void *object)
{
	if (strcmp(primitive, "OVE_TRACE_PRIM_EVENT") || strcmp(action, "OVE_TRACE_ACT_WAIT_EXIT"))
		return;
	check(object == model.event && !model.depth && model.event->waiters == NULL,
	      "completed call retires its automatic record before return");
	/* Record a bad retirement while the automatic record is still alive,
	 * then repair only observer-owned links before any later signal. */
	model.event->waiters = NULL;
	observe_records();
}

static void begin(const char *name, uint64_t ns, __uint128_t start)
{
	case_name = name;
	++cases;
	memset(&model, 0, sizeof(model));
	token.pending = 7;
	model.ns = ns;
	model.clock = start;
#ifndef CONFIG_OVE_ZERO_HEAP
	if (ove_event_create(&model.event) != OVE_OK)
		reject();
#else
	if (ove_event_init(&model.event, &backing) != OVE_OK)
		reject();
#endif
	model.observing = true;
}
static void finish(int expected)
{
	int result = ove_event_wait(model.event, model.ns);
	check(result == expected, "public return matches target latch and elapsed budget");
	check(model.takes == model.count, "every expected positive wait occurs exactly once");
	check(model.drains == 1 && model.registered == 1 && model.retired == 1,
	      "one drain and balanced registration/retirement per operation");
	check(model.event->waiters == NULL && !model.depth,
	      "waiter retired and native protection balanced before return");
	check(model.event->signaled == (unsigned)model.committed_signal,
	      "success consumes its target latch and timeout retains only later signals");
	if (model.committed_signal) {
		check(model.event->signaled == 1, "committed timeout preserves later target latch");
		check(ove_event_wait(model.event, 0) == OVE_OK,
		      "later poll consumes retained latch");
		check(!model.event->signaled, "retained latch is consumed exactly once");
	}
	model.observing = false;
	/* Even a retirement mutation never ends this model token's lifetime.
	 * Clear only test state after recording the failed postcondition. */
	model.event->waiters = NULL;
#ifndef CONFIG_OVE_ZERO_HEAP
	ove_event_destroy(model.event);
#else
	ove_event_deinit(model.event);
#endif
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(false, "early failure survives all later controls");
	const uint64_t ten = UINT64_C(10000000);
	__uint128_t mod = modulus();
	begin("ordinary", ten, 100);
	model.steps[0] = (struct step){1, 1, true};
	model.count = 1;
	finish(OVE_OK);
	begin("one-wrap short success", ten, mod - 6);
	model.steps[0] = (struct step){8, 1, false};
	model.steps[1] = (struct step){1, 1, true};
	model.count = 2;
	finish(OVE_OK);
	begin("one-wrap elapsed beyond budget", ten, mod - 6);
	model.steps[0] = (struct step){12, 1, false};
	model.count = 1;
	finish(OVE_ERR_TIMEOUT);
	for (unsigned wraps = 2; wraps <= 3; ++wraps) {
		begin("extra full wraps below start", ten, mod - 6);
		model.steps[0] = (struct step){(wraps - 1) * mod + 8, 1, false};
		model.count = 1;
		finish(OVE_ERR_TIMEOUT);
	}
	for (unsigned wraps = 1; wraps <= 2; ++wraps)
		for (unsigned tail = 0; tail < 2; ++tail) {
			begin("full wraps equal or above start", ten, 100);
			model.steps[0] = (struct step){wraps * mod + tail, 1, false};
			model.count = 1;
			finish(OVE_ERR_TIMEOUT);
		}
	begin("largest signed finite boundary", UINT64_C(2147483647) * 1000000, 100);
	model.steps[0] = (struct step){1, 1, true};
	model.count = 1;
	finish(OVE_OK);
	for (unsigned power = 31; power <= 32; ++power) {
		begin("signed-horizon finite budget", (UINT64_C(1) << power) * 1000000, 100);
		model.steps[0] = (struct step){1, 1, true};
		model.count = 1;
		finish(OVE_OK);
	}
	begin("finite cap", UINT64_MAX - 1, 100);
	model.steps[0] = (struct step){1, 1, true};
	model.count = 1;
	finish(OVE_OK);
	begin("unrelated hints consume one budget", ten, 100);
	model.steps[0] = (struct step){2, 1, false};
	model.steps[1] = (struct step){3, 1, false};
	model.steps[2] = (struct step){5, 1, false};
	model.count = 3;
	finish(OVE_ERR_TIMEOUT);
	begin("same snapshot updates remaining", ten, 100);
	model.advance_sample = 1;
	model.steps[0] = (struct step){2, 1, false};
	model.steps[1] = (struct step){1, 1, true};
	model.count = 2;
	finish(OVE_OK);
	begin("large delay with target latch", ten, mod - 6);
	model.steps[0] = (struct step){2 * mod + 8, 0, true};
	model.count = 1;
	finish(OVE_OK);
	begin("prior-operation abort marker", ten, 100);
	model.prior_abort = true;
	model.steps[0] = (struct step){1, 1, true};
	model.count = 1;
	finish(OVE_OK);
	begin("native notification abort", ten, 100);
	model.steps[0] = (struct step){OVE_MODEL_ABORT ? 1 : 10, 0, false};
	model.count = 1;
	finish(OVE_ERR_TIMEOUT);
	begin("native timeout loses to target latch", ten, 100);
	model.steps[0] = (struct step){10, 0, true};
	model.count = 1;
	finish(OVE_OK);
	begin("committed timeout keeps later signal", ten, 100);
	model.committed_signal = true;
	model.steps[0] = (struct step){10, 0, false};
	model.count = 1;
	finish(OVE_ERR_TIMEOUT);
	begin("zero poll", 0, 100);
	finish(OVE_ERR_TIMEOUT);
	begin("latched poll", 0, 100);
	ove_event_signal(model.event);
	finish(OVE_OK);
	begin("signal during registration", ten, 100);
	model.register_signal = true;
	finish(OVE_OK);
	begin("latched during elapsed registration", ten, 100);
	model.advance_register = 20;
	model.register_signal = true;
	finish(OVE_OK);
	begin("forever across unrelated hint and long delay", UINT64_MAX, mod - 6);
	model.steps[0] = (struct step){2 * mod + 8, 1, false};
	model.steps[1] = (struct step){1, 1, true};
	model.count = 2;
	finish(OVE_OK);
	begin("defined count frontier", ten, mod - 6);
	model.epoch_bias = INT32_MAX - 3;
	model.steps[0] = (struct step){mod + 8, 1, false};
	model.count = 1;
	finish(OVE_ERR_TIMEOUT);
	check(allocations == frees, "all actual common heap owners are released");
#ifdef CONFIG_OVE_ZERO_HEAP
	check(allocations == 0, "zero-heap observer makes no allocation attempt");
#else
	check(allocations == cases, "heap observer exercises every common Event owner");
#endif
	printf("freertos event accounting: bits=%u abort=%u suspend=%u heap=%u "
	       "cases=%u checks=%u failures=%u\n",
	       OVE_MODEL_TICK_BITS, OVE_MODEL_ABORT, OVE_MODEL_SUSPEND,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       cases, checks, failures);
	return failures != 0;
}
