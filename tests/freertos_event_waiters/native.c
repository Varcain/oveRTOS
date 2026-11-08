/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nested actual calls model live wait invocations. Every token stays initialized;
 * no native task, scheduler or blocking implementation is linked. */
struct model_task {
	unsigned pending, takes;
};
static struct model_task tasks[4];
static unsigned current, depth, checks, failures, cases, allocations, frees;
static uint64_t clock_ticks;
static ove_event_t event;
static const char *case_name;
enum scenario { SEPARATED, POLL_OTHER, EXPIRE_OTHER, THREE, ONE_SIGNAL, COALESCED };
static enum scenario scenario;
#ifdef CONFIG_OVE_ZERO_HEAP
static ove_event_storage_t backing;
#endif

static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s: %s\n", case_name ? case_name : "setup", message);
	}
}
static void reject(void)
{
	fputs("fixture rejected: invalid initialized observer state\n", stderr);
	exit(2);
}
void model_enter_critical(void)
{
	++depth;
}
void model_exit_critical(void)
{
	if (!depth)
		reject();
	--depth;
}
UBaseType_t model_enter_critical_from_isr(void)
{
	unsigned previous = depth;
	model_enter_critical();
	return previous;
}
void model_exit_critical_from_isr(UBaseType_t previous)
{
	model_exit_critical();
	if (depth != previous)
		reject();
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &tasks[current];
}
TickType_t xTaskGetTickCount(void)
{
	return (TickType_t)clock_ticks;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	state->xTimeOnEntering = (TickType_t)clock_ticks;
	state->xOverflowCount = 0;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	bool known = false;
	for (unsigned i = 0; i < 4; ++i)
		known |= task == &tasks[i];
	if (!known)
		reject();
	check(depth != 0, "native hint finishes under notifier exclusion");
	++task->pending;
	return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher)
{
	*higher = pdFALSE;
	(void)xTaskNotifyGive(task);
}
static int wait_as(unsigned task, uint64_t ns)
{
	unsigned previous = current;
	current = task;
	int result = ove_event_wait(event, ns);
	current = previous;
	return result;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	if (clear != pdTRUE || depth)
		reject();
	struct model_task *self = &tasks[current];
	if (!ticks) {
		unsigned value = self->pending;
		self->pending = 0;
		return value;
	}
	uint64_t started = clock_ticks;
	unsigned invocation = self->takes++;
	if (invocation == 0) {
		if (current == 0) {
			uint64_t ns = scenario == POLL_OTHER ? 0 : OVE_MS(10);
			int result = wait_as(1, ns);
			check(result == (scenario == POLL_OTHER || scenario == EXPIRE_OTHER
						 ? OVE_ERR_TIMEOUT
						 : OVE_OK),
			      "nested waiter completes its own expected outcome");
			if (scenario != ONE_SIGNAL)
				ove_event_signal(event);
		} else if (current == 1 && scenario == THREE) {
			check(wait_as(2, OVE_MS(5)) == OVE_OK,
			      "third live waiter consumes its own occurrence");
			ove_event_signal(event);
		} else if (scenario != EXPIRE_OTHER) {
			ove_event_signal(event);
		}
	}
	if (current == 0 && invocation == 0 && scenario != ONE_SIGNAL)
		check(self->pending != 0, "live outer waiter receives a prompt target hint");
	unsigned value = self->pending;
	self->pending = 0;
	if (!value && clock_ticks < started + ticks)
		clock_ticks = started + ticks;
	return value;
}
void *pvPortMalloc(size_t size)
{
	++allocations;
	return malloc(size);
}
void vPortFree(void *pointer)
{
	++frees;
	free(pointer);
}

#include OVE_EVENT_SOURCE

void model_trace(const char *primitive, const char *action, const void *object)
{
	if (strcmp(primitive, "OVE_TRACE_PRIM_EVENT") || strcmp(action, "OVE_TRACE_ACT_WAIT_EXIT"))
		return;
	check(object == event && !depth, "completed wait restores its own exclusion state");
#if OVE_MODEL_EVENT_LIST
	/* Inspect before the automatic record expires. A retirement mutation is
	 * recorded, then only model-owned links are repaired before return so a
	 * later observer never reads expired automatic storage. */
	struct freertos_event_waiter **link = &event->waiters;
	while (*link) {
		if ((*link)->task == &tasks[current]) {
			check(false, "completed invocation retired its own live record");
			*link = (*link)->next;
		} else {
			link = &(*link)->next;
		}
	}
#else
	check(event->waiter != &tasks[current], "completed invocation retired its own slot");
	if (event->waiter == &tasks[current])
		event->waiter = NULL;
#endif
}
static void begin(enum scenario selected, const char *name)
{
	++cases;
	scenario = selected;
	case_name = name;
	clock_ticks = current = depth = 0;
	memset(tasks, 0, sizeof(tasks));
#ifdef CONFIG_OVE_ZERO_HEAP
	if (ove_event_init(&event, &backing) != OVE_OK)
		reject();
#else
	if (ove_event_create(&event) != OVE_OK)
		reject();
#endif
}
static void finish(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	ove_event_deinit(event);
#else
	ove_event_destroy(event);
#endif
	event = NULL;
}
int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "canary"))
		check(false, "early failure survives all later controls");
	const char *names[] = {"separated consumed signals", "poll preserves another wait",
			       "expiry preserves another wait", "three live waits",
			       "one signal has one consumer"};
	for (unsigned i = 0; i < 5; ++i) {
		begin((enum scenario)i, names[i]);
		check(wait_as(0, OVE_MS(100)) == (i == ONE_SIGNAL ? OVE_ERR_TIMEOUT : OVE_OK),
		      "outer wait has the expected public result");
		if (i != ONE_SIGNAL)
			check(clock_ticks == (i == EXPIRE_OTHER ? 10 : 0),
			      "target wakes promptly rather than at the outer timeout");
		check(!depth, "native exclusion is balanced before owner cleanup");
		finish();
	}
	begin(COALESCED, "preposted occurrences coalesce");
	ove_event_signal(event);
	ove_event_signal(event);
	check(wait_as(0, 0) == OVE_OK, "one preposted occurrence is consumed");
	check(wait_as(1, 0) == OVE_ERR_TIMEOUT, "coalescing does not grant a second success");
	finish();
	check(allocations == frees, "common heap owners are balanced");
#ifdef CONFIG_OVE_ZERO_HEAP
	check(!allocations, "caller-owned objects make no allocation attempt");
#else
	check(allocations == cases, "every heap case exercises the common owner");
#endif
	printf("freertos event waiters: bits=32 heap=%u cases=%u checks=%u failures=%u\n",
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       cases, checks, failures);
	return failures != 0;
}
