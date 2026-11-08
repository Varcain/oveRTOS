/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include <event_groups.h>
#include <task.h>
#include <timers.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native objects and queued functions are inert observers. All backing remains
 * physically live through logical retirement/reuse and observation cleanup. */
static ove_eventgroup_storage_t backing[3];
struct native_record {
	EventGroupHandle_t handle;
	bool live;
	uint32_t bits;
};
struct message {
	PendedFunction_t fn;
	void *arg;
	uint32_t value;
};
static struct native_record records[8];
static struct message queue[OVE_MODEL_TIMER_DEPTH];
static unsigned checks, failures, bad, mask, suspended, queued, head, calls, pend_calls;
static unsigned notices, early, yielded, raises, resets, allocations, frees;
static bool in_isr, dispatching, privileged = true, reject, wake, inject_admission;
static void (*suspend_hook)(void), (*resume_hook)(void);
static void *buffers[8];
static ove_eventgroup_t current;
static uint32_t notice_bits;
static unsigned other_notices;
static unsigned expected_yield_mask;
static unsigned trace_enters[2], trace_events[2], trace_returns[2];
static void *trace_owner;
static uint32_t trace_bits;

static void check(bool ok, const char *name)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
BaseType_t model_privileged(void)
{
	return privileged;
}
void model_raise(void)
{
	++raises;
	privileged = true;
}
void model_reset_privilege(void)
{
	++resets;
	privileged = false;
}
void model_enter_critical(void)
{
	if (OVE_MODEL_MPU && !privileged && !in_isr)
		++bad;
	++mask;
}
void model_exit_critical(void)
{
	if (!mask)
		++bad;
	else
		--mask;
}
UBaseType_t model_enter_critical_from_isr(void)
{
	unsigned old = mask;
	++mask;
	return old;
}
void model_exit_critical_from_isr(UBaseType_t value)
{
	if (mask != value + 1)
		++bad;
	mask = value;
}
void vTaskSuspendAll(void)
{
	if (!suspended && suspend_hook) {
		void (*hook)(void) = suspend_hook;
		suspend_hook = NULL;
		hook();
	}
	++suspended;
}
BaseType_t xTaskResumeAll(void)
{
	if (!suspended) {
		++bad;
		return pdFALSE;
	}
	if (!--suspended && resume_hook) {
		void (*hook)(void) = resume_hook;
		resume_hook = NULL;
		hook();
	}
	return pdFALSE;
}
static struct native_record *record(EventGroupHandle_t p)
{
	for (unsigned i = 0; i < 8; ++i)
		if (records[i].handle == p)
			return &records[i];
	++bad;
	return NULL;
}
void model_trace_enter(void *owner, uint32_t bits, bool clear, BaseType_t *wake)
{
	if (!in_isr || !record(owner) || (!clear && (!wake || *wake != pdFALSE)))
		++bad;
	trace_owner = owner;
	trace_bits = bits;
	++trace_enters[clear];
}
void model_trace_event(void *owner, uint32_t bits, bool clear)
{
	if (owner != trace_owner || bits != trace_bits || !in_isr)
		++bad;
	++trace_events[clear];
}
void model_trace_return(BaseType_t result, bool clear)
{
	if (result != pdPASS && result != pdFALSE)
		++bad;
	++trace_returns[clear];
}
EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *p)
{
	for (unsigned i = 0; i < 8; ++i) {
		if (!records[i].handle || records[i].handle == p) {
			records[i] = (struct native_record){p, true, 0};
			return p;
		}
	}
	++bad;
	return NULL;
}
void vEventGroupDelete(EventGroupHandle_t p)
{
	/* Model the old eligible native V1 wrapper, never a privileged opcode. */
	bool old = privileged;
	if (OVE_MODEL_MPU && !old)
		model_raise();
	struct native_record *r = record(p);
	if (!r || !r->live)
		++bad;
	else
		r->live = false;
	if (OVE_MODEL_MPU && !old)
		model_reset_privilege();
}
static ove_eventbits_t enqueue(ove_eventgroup_t group, uint32_t bits)
{
	bool old = in_isr;
	unsigned previous = expected_yield_mask;
	expected_yield_mask = mask;
	in_isr = true;
	ove_eventbits_t result = ove_eventgroup_set_bits_from_isr(group, bits);
	in_isr = old;
	expected_yield_mask = previous;
	return result;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t p, EventBits_t bits)
{
	if (in_isr || mask || (dispatching && !suspended))
		++bad;
	vTaskSuspendAll();
	struct native_record *r = record(p);
	++calls;
	if (!r || !r->live) {
		++bad;
		xTaskResumeAll();
		return 0;
	}
	r->bits |= bits;
	if (inject_admission) {
		inject_admission = false;
		check(enqueue(current, 2) == 2,
		      "queue space admits a command while one daemon slot is active");
	}
	EventBits_t result = r->bits;
	xTaskResumeAll();
	return result;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t p, EventBits_t bits)
{
	if (in_isr || (dispatching && !suspended))
		++bad;
	struct native_record *r = record(p);
	++calls;
	if (!r || !r->live) {
		++bad;
		return 0;
	}
	EventBits_t old = r->bits;
	r->bits &= ~bits;
	return old;
}
EventBits_t xEventGroupGetBits(EventGroupHandle_t p)
{
	struct native_record *r = record(p);
	return r && r->live ? r->bits : 0;
}
static void native_set(void *p, uint32_t bits)
{
	(void)xEventGroupSetBits(p, (EventBits_t)bits);
}
static void native_clear(void *p, uint32_t bits)
{
	(void)xEventGroupClearBits(p, (EventBits_t)bits);
}
BaseType_t xTimerPendFunctionCallFromISR(PendedFunction_t fn, void *p, uint32_t value,
					 BaseType_t *woken)
{
	bool native_pointer = false;
	for (unsigned i = 0; i < 8; ++i)
		native_pointer |= records[i].handle == p;
	if (!native_pointer && !mask)
		++bad;
	++pend_calls;
	if (reject || queued == OVE_MODEL_TIMER_DEPTH)
		return pdFALSE;
	queue[(head + queued++) % OVE_MODEL_TIMER_DEPTH] = (struct message){fn, p, value};
	if (woken)
		*woken = wake;
	return pdPASS;
}
BaseType_t xEventGroupSetBitsFromISR(EventGroupHandle_t p, EventBits_t bits, BaseType_t *woken)
{
#if configUSE_TRACE_FACILITY == 1
	traceENTER_xEventGroupSetBitsFromISR(p, bits, woken);
	traceEVENT_GROUP_SET_BITS_FROM_ISR(p, bits);
#endif
	BaseType_t result = xTimerPendFunctionCallFromISR(native_set, p, (uint32_t)bits, woken);
#if configUSE_TRACE_FACILITY == 1
	traceRETURN_xEventGroupSetBitsFromISR(result);
#endif
	return result;
}
BaseType_t xEventGroupClearBitsFromISR(EventGroupHandle_t p, EventBits_t bits)
{
#if configUSE_TRACE_FACILITY == 1
	traceENTER_xEventGroupClearBitsFromISR(p, bits);
	traceEVENT_GROUP_CLEAR_BITS_FROM_ISR(p, bits);
#endif
	BaseType_t result = xTimerPendFunctionCallFromISR(native_clear, p, (uint32_t)bits, NULL);
#if configUSE_TRACE_FACILITY == 1
	traceRETURN_xEventGroupClearBitsFromISR(result);
#endif
	return result;
}
static void yield_from_isr(BaseType_t value)
{
	if (mask != expected_yield_mask)
		++bad;
	if (value)
		++yielded;
}
#undef portYIELD_FROM_ISR
#define portYIELD_FROM_ISR(value) yield_from_isr(value)

#include OVE_DEFERRED_PROVIDER

void *pvPortMalloc(size_t size)
{
	if (allocations == 8)
		return NULL;
	void *p = malloc(size);
	if (p)
		buffers[allocations++] = p;
	return p;
}
void vPortFree(void *p)
{
	bool found = false;
	for (unsigned i = 0; i < allocations; ++i)
		found |= buffers[i] == p;
	if (!found)
		++bad;
	++frees;
	/* Logical free only; physical bytes remain rooted through observations. */
}
#include "../../src/ove_eventgroup.c"

static ove_eventgroup_t create(unsigned i)
{
	ove_eventgroup_t result = NULL;
	int rc = ove_eventgroup_init(&result, &backing[i]);
	check(rc == OVE_OK && result == &backing[i], "ordinary native initialization");
	return result;
}
static bool stable(const struct message *message)
{
	for (unsigned i = 0; i < 8; ++i)
		if (records[i].handle == message->arg)
			return false;
	return message->arg != NULL;
}
static struct message pop(void)
{
	struct message result = queue[head];
	head = (head + 1) % OVE_MODEL_TIMER_DEPTH;
	--queued;
	return result;
}
static void dispatch(struct message message)
{
	dispatching = true;
	message.fn(message.arg, message.value);
	dispatching = false;
}
static void drain(void)
{
	while (queued)
		dispatch(pop());
}
static void notified(void *data)
{
	if (data != &notices)
		++bad;
	++notices;
	if (in_isr)
		++early;
	else if (mask || suspended)
		++bad;
	/* The old early callback reads virtual observer state, never a task-only
	 * kernel getter from an interrupt. Actual task delivery uses the C API. */
	notice_bits = in_isr ? record(current->handle)->bits : ove_eventgroup_get_bits(current);
}
static void other_notified(void *data)
{
	if (data != &other_notices)
		++bad;
	++other_notices;
}
static void queued_backing(void)
{
	current = create(0);
	for (unsigned op = 0; op < 2; ++op) {
		if (op == 0)
			check(enqueue(current, 1) == 1, "set admission returns its accepted mask");
		else {
			in_isr = true;
			check(ove_eventgroup_clear_bits_from_isr(current, 1) == OVE_OK,
			      "clear admission preserves status");
			in_isr = false;
		}
		check(queued == 1 && stable(&queue[head]),
		      "queued token does not name native backing");
		ove_eventgroup_deinit(current);
		if (stable(&queue[head])) {
			unsigned before = calls;
			drain();
			check(calls == before, "canceled queued work has no native access");
		} else
			(void)pop(); /* Never execute an old token after logical retirement. */
		current = create(0);
	}
}
static void delivery(void)
{
	current = create(0);
	check(ove_eventgroup_set_notify(current, notified, &notices) == OVE_OK,
	      "notification installs before producer exposure");
	check(enqueue(current, 1) == 1, "deferred set is admitted");
	check(notices == 0 && ove_eventgroup_get_bits(current) == 0,
	      "acceptance does not send an early wake");
	drain();
	check(notices == 1 && !early && notice_bits == 1,
	      "daemon publication supplies the observable wake");
	check(bad == 0, "native task update uses scheduler exclusion with interrupts enabled");
}
static void reuse(void)
{
	current = create(0);
	(void)enqueue(current, 1);
	struct message old = pop();
	check(stable(&old), "generation token remains stable");
	ove_eventgroup_deinit(current);
	current = create(0);
	(void)enqueue(current, 2);
	check(old.arg != queue[head].arg, "canceled token is not reused before consumption");
	if (!stable(&old)) {
		(void)pop();
		return;
	}
	dispatch(old);
	check(ove_eventgroup_get_bits(current) == 0, "old generation cannot update reused backing");
	drain();
	check(ove_eventgroup_get_bits(current) == 2, "new generation publishes independently");
}
static void rollback(void)
{
	current = create(0);
	reject = true;
	for (unsigned i = 0; i < OVE_MODEL_TIMER_DEPTH + 3; ++i)
		check(enqueue(current, 1) == 0, "native queue rejection reports failure");
	check(!queued && !notices, "rejection has no delivery or notification");
	reject = false;
	for (unsigned i = 0; i < OVE_MODEL_TIMER_DEPTH; ++i)
		check(enqueue(current, 1) == 1, "failed reservations reopen all capacity");
	check(queued == OVE_MODEL_TIMER_DEPTH, "ordinary native queue capacity is retained");
	drain();
	check(enqueue(current, 2) == 2, "capacity recovers after successful dispatch");
	drain();
}
static void pressure(void)
{
	current = create(0);
	for (unsigned i = 0; i < OVE_MODEL_TIMER_DEPTH; ++i)
		(void)enqueue(current, 1);
	struct message active = pop();
	check(stable(&active), "active daemon message owns a stable token");
	if (!stable(&active)) {
		drain();
		return;
	}
	inject_admission = true;
	dispatch(active);
	check(queued == OVE_MODEL_TIMER_DEPTH,
	      "global capacity includes queued plus active command");
	drain();
	check((ove_eventgroup_get_bits(current) & 3) == 3, "all accepted pressure work publishes");
}
static void replace_at_resume(void)
{
	ove_eventgroup_deinit(current);
	current = create(0);
	check(ove_eventgroup_set_notify(current, other_notified, &other_notices) == OVE_OK,
	      "new generation installs its own callback");
	(void)enqueue(current, 2);
}
static void detached_tail(void)
{
	current = create(0);
	(void)ove_eventgroup_set_notify(current, notified, &notices);
	(void)enqueue(current, 1);
	struct message active = pop();
	check(stable(&active), "saved-tail test uses stable command indirection");
	if (!stable(&active))
		return;
	resume_hook = replace_at_resume;
	dispatch(active);
	check(notices == 1 && other_notices == 0,
	      "retired group uses only the saved callback pair after resume");
	drain();
	check(other_notices == 1 && ove_eventgroup_get_bits(current) == 2,
	      "reused slot and new owner survive the old callback epilogue");
}
static void cancel_at_acquire(void)
{
	ove_eventgroup_deinit(current);
}
static void acquisition(void)
{
	current = create(0);
	(void)enqueue(current, 1);
	struct message active = pop();
	check(stable(&active), "acquisition test uses a stable command token");
	if (!stable(&active))
		return;
	suspend_hook = cancel_at_acquire;
	dispatch(active);
	check(calls == 0, "cancellation before scheduler acquisition suppresses native work");
}
static void zero_and_wake(void)
{
	current = create(0);
	(void)ove_eventgroup_set_notify(current, notified, &notices);
	check(enqueue(current, 0) == 0 && queued == 1, "zero set keeps normal queue admission");
	drain();
	check(!notices && !ove_eventgroup_get_bits(current), "zero set does not notify");
	wake = true;
	in_isr = true;
	check(ove_eventgroup_clear_bits_from_isr(current, 0) == OVE_OK && queued == 1,
	      "zero clear keeps deferred status and ordering");
	in_isr = false;
	check(yielded == 1 && mask == 0, "clear requests the actual wake after mask restoration");
	drain();
	check(!notices && bad == 0, "zero clear never notifies or performs a masked task set");
	mask = 7;
	check(enqueue(current, 1) == 1 && mask == 7 && yielded == 2,
	      "ISR admission restores an already-raised mask before yielding");
	mask = 0;
	drain();
	unsigned enabled = configUSE_TRACE_FACILITY == 1;
	check(trace_enters[0] == 2 * enabled && trace_events[0] == 2 * enabled &&
		      trace_returns[0] == 2 * enabled,
	      "native ISR set tracing keeps entry event and return observations");
	check(trace_enters[1] == enabled && trace_events[1] == enabled &&
		      trace_returns[1] == enabled,
	      "native ISR clear tracing keeps entry event and return observations");
}
static void owners(void)
{
	ove_eventgroup_t a = create(0), b = create(1);
	(void)enqueue(a, 1);
	struct message active = pop();
	(void)enqueue(b, 2);
	check(stable(&active), "global owner test uses stable indirection");
	ove_eventgroup_deinit(a);
	if (stable(&active))
		dispatch(active);
	drain();
	check(ove_eventgroup_get_bits(b) == 2,
	      "canceling one owner preserves another owner's work");
}
static void reenter_notify(void *data)
{
	if (data != &notices)
		++bad;
	++notices;
	if (in_isr) {
		++early;
		return; /* Do not perform task teardown in the original early ISR callback. */
	}
	if (mask || suspended)
		++bad;
	ove_eventgroup_deinit(current);
	current = create(0);
	(void)enqueue(current, 2);
}
static void reentry(void)
{
	current = create(0);
	(void)ove_eventgroup_set_notify(current, reenter_notify, &notices);
	(void)enqueue(current, 1);
	drain();
	check(notices == 1 && !early && bad == 0,
	      "detached notification permits ordinary task teardown and reuse");
	check(ove_eventgroup_get_bits(current) == 2,
	      "notification reentry admits and completes new generation work");
}
static void privilege(void)
{
	current = create(0);
	privileged = !OVE_MODEL_MPU;
	ove_eventgroup_deinit(current);
	check(privileged == !OVE_MODEL_MPU && bad == 0,
	      "eligible task teardown preserves caller privilege and metadata access");
	check(raises == OVE_MODEL_MPU && resets == OVE_MODEL_MPU,
	      "unprivileged V1 route raises and restores exactly once");
	privileged = true;
	current = create(0);
	ove_eventgroup_deinit(current);
	check(privileged && raises == OVE_MODEL_MPU && resets == OVE_MODEL_MPU,
	      "privileged task teardown does not lower its caller");
}
static void heap_retirement(void)
{
#ifdef CONFIG_OVE_ZERO_HEAP
	queued_backing();
#else
	ove_eventgroup_t group = NULL;
	check(ove_eventgroup_create(&group) == OVE_OK, "common heap owner initializes");
	(void)enqueue(group, 1);
	check(stable(&queue[head]), "heap command contains stable indirection");
	ove_eventgroup_destroy(group);
	check(allocations == 1 && frees == 1, "common destroy retains immediate logical free");
	if (stable(&queue[head])) {
		drain();
		check(calls == 0, "no native access follows common logical free");
	} else
		(void)pop();
#endif
}
int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "canary")))
		return 2;
	if (argc == 3)
		check(false, "early failure survives later controls");
	const struct {
		const char *name;
		void (*run)(void);
	} cases[] = {
		{"queued", queued_backing},   {"delivery", delivery},  {"reuse", reuse},
		{"rollback", rollback},	      {"pressure", pressure},  {"tail", detached_tail},
		{"acquisition", acquisition}, {"zero", zero_and_wake}, {"privilege", privilege},
		{"heap", heap_retirement},    {"owners", owners},      {"reentry", reentry}};
	bool found = false;
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		if (!strcmp(cases[i].name, argv[1])) {
			found = true;
			cases[i].run();
		}
	if (!found)
		return 2;
	/* Drop queued observer messages before retiring their physical buffers. */
	queued = 0;
	privileged = true;
	for (unsigned i = 0; i < allocations; ++i)
		free(buffers[i]);
	check(mask == 0 && suspended == 0, "metadata mask and scheduler nesting are restored");
	check(bad == 0, "protected callback and metadata intervals remain valid");
	printf("eventgroup deferred %s: checks=%u failures=%u\n", argv[1], checks, failures);
	return failures != 0;
}
