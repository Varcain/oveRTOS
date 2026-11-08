/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "ove/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Register and instruction-boundary observers only. All target tokens and
 * private records are initialized and retained; no native task is linked. */
struct model_task {
	unsigned notifications;
};
static struct model_task tasks[3];
struct masks {
	uint32_t primask, basepri, faultmask;
};
static struct masks incoming, registers;
static unsigned nesting, initial_nesting, checkpoints, weakened, notifications;
static unsigned yields, cases, checks, failures;
static bool privileged = true, initial_privileged = true, handler;
static const char *case_name, *first_weakened;
static unsigned first_priority;

static void reject(void)
{
	fputs("fixture rejected: invalid initialized mask observer state\n", stderr);
	exit(2);
}

static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s: %s\n", case_name, message);
	}
}

static bool blocked(struct masks state, unsigned priority)
{
	return state.primask != 0 || state.faultmask != 0 ||
	       (state.basepri != 0 && priority >= state.basepri);
}

static void observe(const char *instruction)
{
	++checkpoints;
	for (unsigned priority = 16; priority < 256; priority += 16) {
		if (blocked(incoming, priority) && !blocked(registers, priority)) {
			if (!weakened) {
				first_weakened = instruction;
				first_priority = priority;
			}
			++weakened;
		}
	}
}

uint32_t event_mask_primask(void)
{
	observe("read PRIMASK");
	return registers.primask;
}
uint32_t event_mask_basepri(void)
{
	observe("read BASEPRI");
	return registers.basepri;
}
uint32_t event_mask_faultmask(void)
{
	observe("read FAULTMASK");
	return registers.faultmask;
}
void event_mask_disable_faults(void)
{
	registers.faultmask = 1;
	observe("cpsid f");
}
void event_mask_disable_irqs(void)
{
	registers.primask = 1;
	observe("cpsid i exit envelope");
}
void event_mask_restore_basepri(uint32_t value)
{
	registers.basepri = value;
	observe("restore BASEPRI");
}
void event_mask_restore_primask(uint32_t value)
{
	registers.primask = value;
	observe("restore PRIMASK");
}
void event_mask_restore_faultmask(uint32_t value)
{
	registers.faultmask = value;
	observe("restore FAULTMASK");
}

static void native_raise(void)
{
	if (!privileged)
		reject();
#if OVE_MODEL_ERRATUM
	registers.primask = 1;
	observe("native cpsid i");
#endif
	registers.basepri = configMAX_SYSCALL_INTERRUPT_PRIORITY;
	observe("native fixed BASEPRI write");
#if OVE_MODEL_ERRATUM
	registers.primask = 0;
	observe("native cpsie i");
#endif
}

void model_enter_critical(void)
{
	if (handler)
		reject();
	native_raise();
	++nesting;
}
void model_exit_critical(void)
{
	if (!nesting || handler)
		reject();
	if (--nesting == 0) {
		registers.basepri = 0;
		observe("native outer task exit BASEPRI0");
	}
}
UBaseType_t model_enter_critical_from_isr(void)
{
	if (!handler)
		reject();
	UBaseType_t saved = registers.basepri;
	native_raise();
	return saved;
}
void model_exit_critical_from_isr(UBaseType_t saved)
{
	if (!handler)
		reject();
	registers.basepri = saved;
	observe("native ISR saved BASEPRI write");
}

BaseType_t model_privileged(void)
{
	return privileged ? pdTRUE : pdFALSE;
}
void model_raise(void)
{
	/* Only serviceable restricted entry is represented. */
	if (handler || registers.primask || registers.basepri || registers.faultmask)
		reject();
	privileged = true;
}
void model_lower(void)
{
	privileged = false;
	observe("restore caller mode");
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &tasks[0];
}
TickType_t xTaskGetTickCount(void)
{
	return 0;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	*state = (TimeOut_t){0};
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	if (ticks || clear != pdTRUE)
		reject();
	return 0;
}

static unsigned token(TaskHandle_t task)
{
	for (unsigned i = 0; i < 3; ++i)
		if (task == &tasks[i])
			return i;
	reject();
	return 0;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	unsigned index = token(task);
	if (handler || !nesting || !privileged)
		reject();
	/* The actual native notification has its own critical pair. */
	model_enter_critical();
	++tasks[index].notifications;
	++notifications;
	observe("task notification body");
	model_exit_critical();
	return pdPASS;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *higher)
{
	unsigned index = token(task);
	if (!handler)
		reject();
	UBaseType_t saved = model_enter_critical_from_isr();
	++tasks[index].notifications;
	++notifications;
	/* Head first requests a yield; the next node does not. */
	*higher = index == 1 ? pdTRUE : pdFALSE;
	observe("ISR notification body");
	model_exit_critical_from_isr(saved);
}
static void model_yield(BaseType_t wake)
{
	if (!handler)
		reject();
	check(wake == pdTRUE, "ISR aggregates wake requests across all records");
	check(memcmp(&registers, &incoming, sizeof(incoming)) == 0,
	      "ISR restores original masks before requesting yield");
	++yields;
}
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(wake) model_yield(wake)

void model_trace(const char *primitive, const char *action, const void *object)
{
	(void)primitive;
	(void)action;
	(void)object;
	/* The real current-handle getter reads the native application task tag
	 * under its own critical pair, even when the trace ring drops a record. */
	bool was_privileged = privileged;
	if (!was_privileged)
		model_raise();
	model_enter_critical();
	observe("trace native task tag getter");
	model_exit_critical();
	if (!was_privileged)
		model_lower();
	observe("public trace boundary");
}

#include OVE_EVENT_SOURCE

static void run_case(const char *name, struct masks masks, unsigned depth, bool isr,
		     bool restricted)
{
	case_name = name;
	incoming = registers = masks;
	nesting = initial_nesting = depth;
	privileged = initial_privileged = !restricted;
	handler = isr;
	weakened = checkpoints = notifications = yields = 0;
	first_weakened = NULL;
	memset(tasks, 0, sizeof(tasks));
	ove_event_storage_t storage;
	ove_event_t event;
	check(ove_event_init(&event, &storage) == OVE_OK, "initialize Event storage");
	struct freertos_event_waiter tail = {.task = &tasks[2], .next = NULL};
	struct freertos_event_waiter head = {.task = &tasks[1], .next = &tail};
	event->waiters = &head;
	if (isr)
		ove_event_signal_from_isr(event);
	else
		ove_event_signal(event);
	check(!weakened, "incoming effective interrupt exclusion holds at every boundary");
	if (weakened)
		printf("DETAIL: %s: first=%s priority=0x%02x\n", name, first_weakened,
		       first_priority);
	check(memcmp(&registers, &incoming, sizeof(incoming)) == 0,
	      "return preserves every incoming mask register");
	check(privileged == initial_privileged, "return preserves original caller mode");
	check(nesting == initial_nesting, "native task nesting remains balanced");
	check(notifications == 2 && tasks[1].notifications == 1 && tasks[2].notifications == 1,
	      "all retained live records receive exactly one native hint");
	check(event->signaled == 1, "all hints still represent one shared binary latch");
	check(event->waiters == &head && head.next == &tail && tail.next == NULL,
	      "producer preserves retained registration lifetimes");
	check(checkpoints > 0 && yields == (unsigned)isr, "route and mask observers execute");
	event->waiters = NULL;
	ove_event_deinit(event);
	++cases;
}

static void run_step_case(const char *name, struct masks masks, unsigned depth, bool restricted)
{
	case_name = name;
	incoming = registers = masks;
	nesting = initial_nesting = depth;
	privileged = initial_privileged = !restricted;
	handler = false;
	weakened = checkpoints = notifications = yields = 0;
	first_weakened = NULL;
	ove_event_storage_t storage;
	ove_event_t event;
	check(ove_event_init(&event, &storage) == OVE_OK, "initialize Event for private step");
	struct freertos_event_waiter tail = {.task = &tasks[2], .next = NULL};
	struct freertos_event_waiter self = {.task = &tasks[0], .next = NULL};
	event->waiters = &tail;
	/* Internal operation proof, including raw incoming masks. This does not
	 * admit a masked caller to the public blocking/poll API. */
	check(!event_wait_step(event, &self, true, false), "publication alone consumes no latch");
	check(event->waiters == &self && self.next == &tail,
	      "publish retains both initialized records");
	event->signaled = 1;
	check(event_wait_step(event, &self, false, false), "consume one shared latch");
	check(event->waiters == &tail && event->signaled == 0,
	      "consumption retires only the current invocation");
	check(!event_wait_step(event, &self, true, true),
	      "poll retirement consumes no absent latch");
	check(event->waiters == &tail && tail.next == NULL,
	      "retirement preserves another live record");
	check(!weakened, "private step preserves incoming effective exclusion at every boundary");
	if (weakened)
		printf("DETAIL: %s: first=%s priority=0x%02x\n", name, first_weakened,
		       first_priority);
	check(memcmp(&registers, &incoming, sizeof(incoming)) == 0,
	      "private step restores every incoming mask register");
	check(privileged == initial_privileged, "private step restores original caller mode");
	check(nesting == initial_nesting, "private step balances native nesting");
	check(notifications == 0 && yields == 0, "private step sends no producer hints");
	check(checkpoints != 0, "private step mask observers execute");
	event->waiters = NULL;
	ove_event_deinit(event);
	++cases;
}

static void public_poll(bool restricted)
{
	case_name = "public-prelatched-poll";
	incoming = registers = (struct masks){0};
	nesting = 0;
	privileged = !restricted;
	handler = false;
	weakened = checkpoints = notifications = yields = 0;
	ove_event_storage_t storage;
	ove_event_t event;
	check(ove_event_init(&event, &storage) == OVE_OK, "initialize public poll Event");
	ove_event_signal(event);
	check(ove_event_wait(event, 0) == OVE_OK, "public poll consumes a prelatched occurrence");
	check(event->signaled == 0 && event->waiters == NULL,
	      "public poll returns without a live record");
	check(privileged == !restricted, "public poll preserves serviceable caller mode");
	check(!registers.primask && !registers.basepri && !registers.faultmask && !nesting &&
		      !weakened,
	      "public poll returns with enabled interrupts and balanced native nesting");
	ove_event_deinit(event);
	++cases;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		struct masks masks;
		unsigned depth;
	} vectors[] = {
		{"unmasked", {0, 0, 0}, 0},
		{"kernel-threshold", {0, 0x80, 0}, 0},
		{"weaker-basepri", {0, 0xc0, 0}, 0},
		{"stronger-basepri", {0, 0x40, 0}, 0},
		{"primask", {1, 0, 0}, 0},
		{"primask-stronger-basepri", {1, 0x40, 0}, 0},
		{"faultmask", {0, 0, 1}, 0},
		{"faultmask-stronger-basepri", {0, 0x40, 1}, 0},
		{"all-masks", {1, 0x40, 1}, 0},
		{"native-nested", {0, 0x80, 0}, 1},
		{"native-twice-nested", {0, 0x80, 0}, 2},
		{"native-nested-stronger-basepri", {0, 0x40, 0}, 1},
	};
	for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
		char name[96];
		for (unsigned route = 0; route < 2; ++route) {
			/* A kernel-aware IRQ cannot enter an already masked native task
			 * critical section. Other ISR vectors install masks in handler. */
			if (route && vectors[i].depth)
				continue;
			snprintf(name, sizeof(name), "%s-%s", route ? "isr" : "task",
				 vectors[i].name);
			run_case(name, vectors[i].masks, vectors[i].depth, route != 0, false);
		}
		snprintf(name, sizeof(name), "private-step-%s", vectors[i].name);
		run_step_case(name, vectors[i].masks, vectors[i].depth, false);
	}
#if OVE_MODEL_MPU
	run_case("restricted-serviceable-svc", (struct masks){0}, 0, false, true);
	run_step_case("restricted-private-step", (struct masks){0}, 0, true);
#endif
	public_poll(OVE_MODEL_MPU != 0);
	if ((argc > 1 && strcmp(argv[1], "canary") == 0) ||
	    getenv("OVE_FREERTOS_EVENT_MASK_CANARY") != NULL) {
		case_name = "canary";
		check(false, "controlled mask-observer failure reaches the caller");
	}
	printf("freertos event masks: erratum=%u mpu=%u heap=%u cases=%u checks=%u failures=%u\n",
	       OVE_MODEL_ERRATUM, OVE_MODEL_MPU,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       cases, checks, failures);
	return failures != 0;
}
