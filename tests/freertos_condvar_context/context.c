/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "ove/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These are retained observer tokens, never native task/MPU objects. Raw
 * privilege violations are counted in scalar spies, not executed on ARM. */
struct model_task {
	unsigned pending;
	bool privileged;
};
struct masks {
	uint32_t primask, basepri, faultmask;
};
static struct model_task tasks[3];
static unsigned current, nesting, initial_nesting, failures, checks, cases;
static unsigned forbidden, unprivileged_regions, weakened, notices, repairs;
static bool privileged = true, notify_active, given, release_error, reacquire_error;
static struct masks registers, incoming;
static ove_condvar_t cv;
static ove_mutex_t mutex;
static const char *case_name;
enum schedule { TIMEOUT, PENDING, SIGNAL, BROADCAST, FOREIGN, LATE };
static enum schedule schedule;

#include OVE_CV_SOURCE

static struct condvar_waiter *live_self, *foreign_head;

static void reject(void)
{
	fputs("fixture rejected: invalid live CV observer state\n", stderr);
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
	return state.primask || state.faultmask || (state.basepri && priority >= state.basepri);
}
static void observe(void)
{
	for (unsigned priority = 16; priority < 256; priority += 16)
		if (blocked(incoming, priority) && !blocked(registers, priority))
			++weakened;
}
uint32_t event_mask_primask(void)
{
	observe();
	return registers.primask;
}
uint32_t event_mask_basepri(void)
{
	observe();
	return registers.basepri;
}
uint32_t event_mask_faultmask(void)
{
	observe();
	return registers.faultmask;
}
void event_mask_disable_faults(void)
{
	registers.faultmask = 1;
	observe();
}
void event_mask_disable_irqs(void)
{
	registers.primask = 1;
	observe();
}
void event_mask_restore_basepri(uint32_t value)
{
	registers.basepri = value;
	observe();
}
void event_mask_restore_primask(uint32_t value)
{
	registers.primask = value;
	observe();
}
void event_mask_restore_faultmask(uint32_t value)
{
	registers.faultmask = value;
	observe();
}
BaseType_t model_privileged(void)
{
	return privileged ? pdTRUE : pdFALSE;
}
void model_raise(void)
{
	if (registers.primask || registers.basepri || registers.faultmask)
		reject();
	privileged = true;
}
void model_lower(void)
{
	privileged = false;
	observe();
}
void model_enter_critical(void)
{
	/* Option1 promotes only inside the native function and restores before
	 * its caller resumes. Neither option grants the adapter an outer scope. */
	if (!privileged) {
		forbidden += !OVE_MODEL_ALLOW;
		++unprivileged_regions;
	}
#if OVE_MODEL_ERRATUM
	registers.primask = 1;
	observe();
#endif
	registers.basepri = configMAX_SYSCALL_INTERRUPT_PRIORITY;
	observe();
#if OVE_MODEL_ERRATUM
	registers.primask = 0;
	observe();
#endif
	++nesting;
}
static bool linked(struct condvar_waiter *self)
{
	unsigned limit = 0;
	for (struct condvar_waiter *p = cv->head; p; p = p->next) {
		if (++limit > 3)
			reject();
		if (p == self)
			return true;
	}
	return false;
}
static void retire_checkpoint(void)
{
	if (!live_self)
		return;
	bool retained = linked(live_self);
	check(!retained, "own record is retired while its frame is live");
	if (retained) {
		/* Only the model-owned, still-live invocation is repaired. */
		struct condvar_waiter **p = &cv->head;
		while (*p != live_self)
			p = &(*p)->next;
		*p = live_self->next;
		++repairs;
	}
	live_self = NULL;
}
void model_exit_critical(void)
{
	if (!nesting)
		reject();
	if (!privileged)
		++unprivileged_regions;
	/* On release failure this is a pre-return, live-frame checkpoint.
	 * A whole-retirement-omission mutant is ineligible for that scenario. */
	if (given && release_error && !notify_active && nesting == 1)
		retire_checkpoint();
	if (--nesting == 0) {
		registers.basepri = 0;
		observe();
	}
}
static void boundary(void)
{
	check(privileged == tasks[current].privileged, "native boundary has caller mode");
	check(!nesting && !registers.primask && !registers.basepri && !registers.faultmask,
	      "release, take and reacquire are outside protection");
}
static bool api_enter(void)
{
	bool saved = privileged;
	privileged = true;
	model_enter_critical();
	return saved;
}
static void api_leave(bool saved)
{
	model_exit_critical();
	privileged = saved;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
	return &tasks[current];
}
TickType_t xTaskGetTickCount(void)
{
	return 0;
}
void vTaskSetTimeOutState(TimeOut_t *state)
{
	*state = (TimeOut_t){0};
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
	boundary();
	if (sem != mutex->sem || !sem->initialized || sem->owner != &tasks[current] ||
	    sem->depth != 1)
		reject();
	if (current != 0 || !cv->head || cv->head->task != &tasks[current])
		reject();
	live_self = cv->head;
	bool saved = api_enter();
	if (!release_error) {
		sem->owner = NULL;
		sem->depth = 0;
	}
	api_leave(saved);
	given = true;
	return release_error ? pdFALSE : pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
	boundary();
	retire_checkpoint();
	if (sem != mutex->sem || !sem->initialized || sem->owner || ticks != portMAX_DELAY)
		reject();
	bool saved = api_enter();
	if (!reacquire_error) {
		sem->owner = &tasks[current];
		sem->depth = 1;
	}
	api_leave(saved);
	return reacquire_error ? pdFALSE : pdTRUE;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
	unsigned index;
	for (index = 0; index < 3 && task != &tasks[index]; ++index) {
	}
	if (index == 3 || !notify_active)
		reject();
	check(privileged, "notification argument and foreign traversal have an enclosing scope");
	check(nesting != 0, "notification retains the live record through native use");
	bool saved = api_enter();
	++tasks[index].pending;
	++notices;
	api_leave(saved);
	return pdPASS;
}
static void post(bool all)
{
	notify_active = true;
	if (all)
		ove_condvar_broadcast(cv);
	else
		ove_condvar_signal(cv);
	notify_active = false;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
	boundary();
	if (clear != pdTRUE || current != 0)
		reject();
	if (ticks && (schedule == SIGNAL || schedule == BROADCAST || schedule == LATE)) {
		current = 2;
		privileged = tasks[2].privileged;
		post(schedule == BROADCAST);
		check(privileged == tasks[2].privileged,
		      "foreign producer restores its caller mode");
		current = 0;
		privileged = tasks[0].privileged;
		if (schedule == LATE) {
			schedule = TIMEOUT;
			return 0;
		}
	}
	if (ticks && schedule == FOREIGN) {
		/* Initialized automatic observer records remain alive in exercise().
		 * This models a newer publication, not an actual native task grant. */
		foreign_head->next->next = cv->head;
		cv->head = foreign_head;
	}
	bool saved = api_enter();
	uint32_t result = tasks[0].pending;
	tasks[0].pending = 0;
	api_leave(saved);
	return result;
}
void model_trace(const char *primitive, const char *action, const void *object)
{
	(void)primitive;
	(void)object;
	if (!strcmp(action, "OVE_TRACE_ACT_POST")) {
		/* Model the real task-tag getter's nested raw critical pair. */
		check(privileged, "post getter has an enclosing privileged scope");
		model_enter_critical();
		model_exit_critical();
	} else {
		boundary();
	}
}
static void reset(const char *name, bool restricted)
{
	case_name = name;
	++cases;
	memset(tasks, 0, sizeof(tasks));
	for (unsigned i = 0; i < 3; ++i)
		tasks[i].privileged = !restricted;
	current = nesting = initial_nesting = 0;
	privileged = !restricted;
	registers = incoming = (struct masks){0};
	forbidden = unprivileged_regions = weakened = notices = repairs = 0;
	given = release_error = reacquire_error = notify_active = false;
	live_self = foreign_head = NULL;
}
static void exercise(enum schedule selected, bool restricted, unsigned error)
{
	reset("wait", restricted);
	schedule = selected;
	release_error = error == 1;
	reacquire_error = error == 2;
	ove_mutex_storage_t ms = {0};
	ove_condvar_storage_t cs = {0};
	mutex = &ms;
	ms.sem = &ms.static_sem;
	ms.static_sem.initialized = true;
	ms.static_sem.owner = &tasks[0];
	ms.static_sem.depth = 1;
	if (ove_condvar_init(&cv, &cs))
		reject();
	struct condvar_waiter a = {.task = &tasks[1]}, b = {.task = &tasks[2]};
	a.next = &b;
	foreign_head = &a;
	tasks[0].pending = selected == PENDING;
	bool held = false;
	int result = ove_condvar_wait_checked(cv, mutex, selected == TIMEOUT ? 0 : 1000000, &held);
	check(result == (error ? OVE_ERR_BUSY
			 : selected == PENDING || selected == SIGNAL || selected == BROADCAST ||
					 selected == LATE
				 ? OVE_OK
				 : OVE_ERR_TIMEOUT),
	      "status preserves timeout, spurious wake and native failures");
	check(held == !reacquire_error, "checked outcome preserves actual ownership");
	check(privileged == tasks[0].privileged && !nesting, "wait restores caller state");
	check(!forbidden && !unprivileged_regions, "every list region has enclosing privilege");
	check(!weakened && !repairs, "no exclusion or live-frame repair failure");
	check(!live_self, "live snapshot cleared before returning the scenario");
	check(selected == FOREIGN && !release_error ? cv->head == &a && a.next == &b && !b.next
						    : cv->head == NULL,
	      "only this invocation is retired");
	/* Both foreign observer records are still live and are now detached. */
	cs.head = NULL;
	ms.static_sem.owner = NULL;
	ms.static_sem.depth = 0;
	ove_condvar_deinit(cv);
}
static void masks_case(struct masks entry, unsigned depth, bool all, bool restricted)
{
	reset("notify masks", restricted);
	ove_condvar_storage_t cs = {0};
	if (ove_condvar_init(&cv, &cs))
		reject();
	struct condvar_waiter b = {.task = &tasks[1]}, a = {.task = &tasks[0], .next = &b};
	cs.head = &a;
	registers = incoming = entry;
	nesting = initial_nesting = depth;
	post(all);
	check(privileged == !restricted, "producer restores original caller mode");
	check(nesting == initial_nesting, "producer balances native nesting");
	check(!memcmp(&registers, &incoming, sizeof(registers)), "producer restores exact masks");
	check(!weakened, "incoming exclusion holds at every observed instruction boundary");
	check(!forbidden && !unprivileged_regions, "foreign traversal stays privileged");
	check(notices == (all ? 2u : 1u), "signal and broadcast retain notification cardinality");
	check(cs.head == (all ? NULL : &b), "notification retires the selected live records");
	cs.head = NULL;
	registers = incoming = (struct masks){0};
	nesting = 0;
	post(all);
	ove_condvar_deinit(cv);
}
int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "normal") && strcmp(argv[1], "canary")))
		reject();
	if (!strcmp(argv[1], "canary")) {
		case_name = "early canary";
		check(false, "failure survives all later controls");
	}
	for (unsigned restricted = 0; restricted <= OVE_MODEL_MPU; ++restricted) {
		for (enum schedule s = TIMEOUT; s <= LATE; ++s)
			exercise(s, restricted, 0);
		exercise(TIMEOUT, restricted, 1);
		exercise(PENDING, restricted, 2);
		for (unsigned all = 0; all < 2; ++all)
			masks_case((struct masks){0}, 0, all, restricted);
	}
	const struct masks entries[] = {{0, 128, 0}, {1, 0, 0},	  {0, 64, 0}, {0, 0, 1},
					{1, 64, 1},  {0, 192, 0}, {1, 192, 0}};
	for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i)
		for (unsigned all = 0; all < 2; ++all)
			masks_case(entries[i], i == 0 ? 1 : 0, all, false);
	printf("freertos cv context: cases=%u checks=%u failures=%u\n", cases, checks, failures);
	return failures != 0;
}
