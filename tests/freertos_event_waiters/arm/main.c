/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if OVE_NATIVE_TRACE
#include "ove/trace.h"
#include "ove_trace_ring.h"
#endif

struct job {
	uint64_t timeout;
	atomic_uint done, progress;
	unsigned restricted, producer, repeat, entry, exit, blocks, bad_boundary;
	int result[2];
};
struct user_region {
	ove_event_storage_t storage;
	ove_event_t event;
	struct job jobs[4];
};
_Static_assert(sizeof(struct user_region) <= 512, "all shared worker state fits its MPU region");
static struct user_region user __attribute__((aligned(512)));
static StaticTask_t worker_tcbs[4], supervisor_tcb;
static StackType_t worker_stacks[4][512] __attribute__((aligned(2048)));
static StackType_t supervisor_stack[1024];
static TaskHandle_t handles[4];
static unsigned checks, failures, cases, workers, minimum_headroom = 2048;
static const char *case_name;
static volatile unsigned irq_calls, irq_bad_masks, observer_calls, irq_nested;
static volatile unsigned irq_pending_before, irq_pending_after;

static unsigned control(void)
{
	unsigned value;
	__asm volatile("mrs %0, control" : "=r"(value));
	return value & 1;
}
static unsigned primask(void)
{
	unsigned value;
	__asm volatile("mrs %0, primask" : "=r"(value));
	return value;
}
static unsigned basepri(void)
{
	unsigned value;
	__asm volatile("mrs %0, basepri" : "=r"(value));
	return value;
}
static unsigned faultmask(void)
{
	unsigned value;
	__asm volatile("mrs %0, faultmask" : "=r"(value));
	return value;
}
static void masks(unsigned p, unsigned b, unsigned f)
{
	/* This fixture helper is called only by the privileged supervisor/IRQ. */
	__asm volatile("cpsid i\n\tmsr basepri, %0\n\tdsb\n\tisb\n\tmsr primask, %1\n\t"
		       "msr faultmask, %2\n\tisb"
		       :
		       : "r"(b), "r"(p), "r"(f)
		       : "memory");
}
void event_waiters_target_reject(void)
{
	fputs("native Event fixture rejected an invalid setup or completion state\n", stdout);
	semihosting_exit(3);
}
void event_waiters_target_assert(unsigned line)
{
	printf("native Event fixture assertion: line=%u case=%s\n", line,
	       case_name ? case_name : "startup");
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	event_waiters_target_reject();
}
static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s: %s\n", case_name, message);
	}
}

static void observe_block(TickType_t ticks)
{
	struct job *job = pvTaskGetThreadLocalStoragePointer(NULL, 0);
	if (!job || !ticks)
		return;
	++job->blocks;
	if (control() != job->restricted || primask() || basepri() || faultmask())
		++job->bad_boundary;
}
#ifdef OVE_MPU_V1
uint32_t __real_MPU_ulTaskGenericNotifyTake(UBaseType_t, BaseType_t, TickType_t);
uint32_t __wrap_MPU_ulTaskGenericNotifyTake(UBaseType_t index, BaseType_t clear, TickType_t ticks)
{
	observe_block(ticks);
	return __real_MPU_ulTaskGenericNotifyTake(index, clear, ticks);
}
#else
uint32_t __real_ulTaskGenericNotifyTake(UBaseType_t, BaseType_t, TickType_t);
uint32_t __wrap_ulTaskGenericNotifyTake(UBaseType_t index, BaseType_t clear, TickType_t ticks)
{
	observe_block(ticks);
	return __real_ulTaskGenericNotifyTake(index, clear, ticks);
}
#endif

static void worker(void *data)
{
	struct job *job = data;
	vTaskSetThreadLocalStoragePointer(NULL, 0, job);
	job->entry = control();
	if (job->producer) {
		ove_event_signal(user.event);
	} else {
		for (unsigned i = 0; i < job->repeat; ++i) {
			job->result[i] = ove_event_wait(user.event, job->timeout);
			atomic_store_explicit(&job->progress, i + 1, memory_order_release);
		}
	}
	job->exit = control();
	atomic_store_explicit(&job->done, 1, memory_order_release);
	vTaskSuspend(NULL);
	event_waiters_target_reject();
}
static void start(unsigned index, bool restricted, uint64_t timeout, unsigned repeat, bool producer)
{
	struct job *job = &user.jobs[index];
	job->timeout = timeout;
	job->restricted = restricted;
	job->repeat = repeat;
	job->producer = producer;
	job->result[0] = job->result[1] = OVE_ERR_BUSY;
	atomic_init(&job->done, 0);
	atomic_init(&job->progress, 0);
	TaskHandle_t task = NULL;
#ifdef OVE_MPU_V1
	if (restricted) {
		TaskParameters_t parameters = {
			.pvTaskCode = worker,
			.pcName = "restricted",
			.usStackDepth = 512,
			.pvParameters = job,
			.uxPriority = 4,
			.puxStackBuffer = worker_stacks[index],
			.xRegions = {{.pvBaseAddress = &user,
				      .ulLengthInBytes = 512,
				      .ulParameters = portMPU_REGION_READ_WRITE |
						      portMPU_REGION_EXECUTE_NEVER}},
			.pxTaskBuffer = &worker_tcbs[index],
		};
		if (xTaskCreateRestrictedStatic(&parameters, &task) != pdPASS)
			event_waiters_target_reject();
	} else {
		task = xTaskCreateStatic(worker, "privileged", 512, job, 4 | portPRIVILEGE_BIT,
					 worker_stacks[index], &worker_tcbs[index]);
	}
#else
	(void)restricted;
	task = xTaskCreateStatic(worker, "ordinary", 512, job, 4, worker_stacks[index],
				 &worker_tcbs[index]);
#endif
	if (!task)
		event_waiters_target_reject();
	handles[index] = task;
	++workers;
	check(eTaskGetState(task) == (producer ? eSuspended : eBlocked),
	      "initialized native worker reaches its public boundary");
}
static unsigned done(unsigned count)
{
	unsigned total = 0;
	for (unsigned i = 0; i < count; ++i)
		total += atomic_load_explicit(&user.jobs[i].done, memory_order_acquire);
	return total;
}
static bool await_done(unsigned count, unsigned wanted, unsigned ticks)
{
	for (unsigned i = 0; i < ticks && done(count) < wanted; ++i)
		vTaskDelay(1);
	return done(count) == wanted;
}
static void begin(const char *name)
{
	case_name = name;
	memset(&user, 0, sizeof(user));
	memset(handles, 0, sizeof(handles));
	check(ove_event_init(&user.event, &user.storage) == OVE_OK, "initialize accessible Event");
}
static void post(unsigned count, unsigned wanted)
{
	ove_event_signal(user.event);
	check(await_done(count, wanted, 20),
	      "separated occurrence promptly releases one live waiter");
}
static void finish(unsigned count)
{
	/* Capture every prompt failure before recovery. The public latch and
	 * extra native hints safely release even a valid old single-slot worker. */
	for (unsigned turn = 0; turn < count + 2 && done(count) != count; ++turn) {
		ove_event_signal(user.event);
		for (unsigned i = 0; i < count; ++i)
			if (!atomic_load_explicit(&user.jobs[i].done, memory_order_acquire))
				xTaskNotifyGive(handles[i]);
		vTaskDelay(1);
	}
	if (done(count) != count)
		event_waiters_target_reject();
	for (unsigned i = 0; i < 4; ++i) {
		if (!handles[i])
			continue;
		struct job *job = &user.jobs[i];
		if (!atomic_load_explicit(&job->done, memory_order_acquire) ||
		    eTaskGetState(handles[i]) != eSuspended)
			event_waiters_target_reject();
		check(job->entry == job->restricted && job->exit == job->restricted &&
			      job->bad_boundary == 0 && (job->producer || job->blocks != 0),
		      "caller mode and enabled masks restored before every positive block and return");
		unsigned headroom = uxTaskGetStackHighWaterMark(handles[i]) * sizeof(StackType_t);
		if (headroom < minimum_headroom)
			minimum_headroom = headroom;
		check(headroom >= 256, "bounded native worker retains stack headroom");
		vTaskDelete(handles[i]);
		handles[i] = NULL;
	}
	ove_event_deinit(user.event);
#if OVE_NATIVE_TRACE
	struct ove_trace_record records[8];
	unsigned posts = 0;
	size_t available;
	while ((available = ove_trace_ring_drain(records, 8)) != 0)
		for (size_t i = 0; i < available; ++i)
			if (records[i].kind == OVE_TRACE_KIND_MARK &&
			    records[i].code == ((OVE_TRACE_PRIM_EVENT << 4) | OVE_TRACE_ACT_POST))
				++posts;
	check(posts != 0 && ove_trace_ring_dropped_get() == 0,
	      "actual native trace retains Event post markers without drops");
#endif
	check(control() == 0 && primask() == 0 && basepri() == 0 && faultmask() == 0,
	      "supervisor remains privileged and unmasked after cleanup");
	++cases;
}
static void separated(unsigned count, bool restricted)
{
	begin(count == 2 ? "two-separated" : "three-separated");
	for (unsigned i = 0; i < count; ++i)
		start(i, restricted, OVE_MS(500), 1, false);
	for (unsigned i = 1; i <= count; ++i)
		post(count, i);
	check(done(count) == count, "all separated occurrences complete before recovery");
	finish(count);
}
static void one_signal(bool restricted)
{
	begin("one-occurrence");
	start(0, restricted, OVE_MS(500), 1, false);
	start(1, restricted, OVE_MS(500), 1, false);
	post(2, 1);
	vTaskDelay(2);
	check(done(2) == 1, "one binary occurrence permits only one successful consumer");
	post(2, 2);
	finish(2);
}
static void poll(bool restricted)
{
	begin("poll-preserves-live-waiters");
	start(0, restricted, OVE_MS(500), 1, false);
	start(1, restricted, OVE_MS(500), 1, false);
	check(ove_event_wait(user.event, 0) == OVE_ERR_TIMEOUT, "poll has no occurrence");
	post(2, 1);
	post(2, 2);
	finish(2);
}
static void retire(unsigned selected, bool restricted, bool abort)
{
	begin(abort ? "abort-retirement" : "expiry-retirement");
	for (unsigned i = 0; i < 3; ++i)
		start(i, restricted, i == selected && !abort ? OVE_MS(5) : OVE_MS(500), 1, false);
#if OVE_NATIVE_ABORT
	if (abort)
		check(xTaskAbortDelay(handles[selected]) == pdPASS,
		      "valid initialized blocked worker accepts native abort");
#else
	(void)abort;
#endif
	check(await_done(3, 1, 20) &&
		      atomic_load_explicit(&user.jobs[selected].done, memory_order_acquire) == 1 &&
		      user.jobs[selected].result[0] == OVE_ERR_TIMEOUT,
	      "selected head middle or tail retires through its public return");
	post(3, 2);
	post(3, 3);
	finish(3);
}
static void suspended(bool restricted)
{
	begin("suspended-head");
	start(0, restricted, OVE_MS(500), 1, false);
	start(1, restricted, OVE_MS(500), 1, false);
	vTaskSuspend(handles[1]);
	ove_event_signal(user.event);
	check(await_done(2, 1, 20) &&
		      atomic_load_explicit(&user.jobs[0].done, memory_order_acquire) == 1,
	      "suspended head does not absorb the only useful hint");
	vTaskResume(handles[1]);
	check(done(2) <= 1, "resumed stale hint grants no second consumer");
	post(2, 2);
	finish(2);
}
static void hint(bool restricted)
{
	begin("unrelated-hint");
	for (unsigned i = 0; i < 3; ++i)
		start(i, restricted, OVE_MS(500), 1, false);
	xTaskNotifyGive(handles[0]);
	check(done(3) == 0 && eTaskGetState(handles[0]) == eBlocked,
	      "unrelated native hint reblocks without consuming an occurrence");
	for (unsigned i = 1; i <= 3; ++i)
		post(3, i);
	finish(3);
}
static void reuse(bool restricted)
{
	begin("same-worker-reuse");
	start(0, restricted, OVE_MS(500), 2, false);
	ove_event_signal(user.event);
	check(atomic_load_explicit(&user.jobs[0].progress, memory_order_acquire) == 1 &&
		      eTaskGetState(handles[0]) == eBlocked,
	      "returned first invocation safely registers its next wait");
	post(1, 1);
	check(user.jobs[0].result[0] == OVE_OK && user.jobs[0].result[1] == OVE_OK,
	      "same native worker consumes two separated occurrences");
	finish(1);
}

void UART0TX_IRQHandler(void)
{
	/* Priority0x60 is above the kernel threshold: observe only, no API. */
	++observer_calls;
}
void UART0RX_IRQHandler(void)
{
	irq_pending_before = (*(volatile uint32_t *)0xe000ed04 >> 28) & 1;
	unsigned p = primask(), b = basepri(), f = faultmask();
	if (irq_nested)
		masks(1, 0x40, f);
	unsigned expected_p = primask(), expected_b = basepri(), expected_f = faultmask();
	ove_event_signal_from_isr(user.event);
	irq_pending_after = (*(volatile uint32_t *)0xe000ed04 >> 28) & 1;
	if (primask() != expected_p || basepri() != expected_b || faultmask() != expected_f)
		++irq_bad_masks;
	++irq_calls;
	masks(p, b, f);
}
static void irq(bool restricted, bool nested)
{
	begin(nested ? "genuine-irq-nested-mask" : "genuine-irq");
	start(0, restricted, OVE_MS(500), 1, false);
	start(1, restricted, OVE_MS(500), 1, false);
	/* The head requests a higher-priority wake, the tail does not. */
	vTaskPrioritySet(handles[0], 1);
	irq_calls = irq_bad_masks = 0;
	irq_nested = nested;
	*(volatile uint32_t *)0xe000e200 = 1;
	__asm volatile("dsb\n\tisb" : : : "memory");
	check(await_done(2, 1, 20) && irq_calls == 1 && irq_bad_masks == 0,
	      "genuine kernel-aware IRQ releases one waiter and preserves its mask");
	check(!irq_pending_before && irq_pending_after,
	      "later lower-priority waiter does not erase the IRQ yield request");
	post(2, 2);
	finish(2);
}
static void producer(bool restricted, bool producer_restricted)
{
	begin("native-producer-mode");
	start(0, restricted, OVE_MS(500), 1, false);
	start(1, restricted, OVE_MS(500), 1, false);
	start(3, producer_restricted, 0, 0, true);
	check(await_done(2, 1, 20), "ordinary public producer releases a foreign-stack waiter");
	post(2, 2);
	finish(2);
}
static void raw_mask(unsigned p, unsigned b, unsigned f)
{
	begin(p ? "raw-primask" : f ? "raw-faultmask" : "raw-stronger-basepri");
	start(0, false, OVE_MS(500), 1, false);
	observer_calls = 0;
	masks(p, b, f);
	*(volatile uint32_t *)0xe000e200 = 2;
	__asm volatile("dsb\n\tisb" : : : "memory");
	ove_event_signal(user.event);
	bool same = primask() == p && basepri() == b && faultmask() == f;
	unsigned prematurely_ran = observer_calls;
	/* Always restore and complete real workers before asserting failures. */
	masks(0, 0, 0);
	check(same, "task Event restores all incoming raw mask registers");
	check(prematurely_ran == 0, "pending harmless observer remains masked throughout Event");
	check(observer_calls == 1 && await_done(1, 1, 20),
	      "explicit caller unmask releases pending observer and real waiter");
	finish(1);
}
static void native_contexts(void)
{
	begin("native-nesting");
	start(0, false, OVE_MS(500), 1, false);
	taskENTER_CRITICAL();
	taskENTER_CRITICAL();
	ove_event_signal(user.event);
	bool held = basepri() == configMAX_SYSCALL_INTERRUPT_PRIORITY && done(1) == 0;
	taskEXIT_CRITICAL();
	taskEXIT_CRITICAL();
	check(held && await_done(1, 1, 20),
	      "nested native critical producer defers wake until unlock");
	finish(1);
	begin("scheduler-suspended");
	start(0, false, OVE_MS(500), 1, false);
	vTaskSuspendAll();
	ove_event_signal(user.event);
	bool deferred = done(1) == 0;
	(void)xTaskResumeAll();
	check(deferred && await_done(1, 1, 20),
	      "scheduler-suspended producer defers wake until resume");
	finish(1);
}
static void supervisor(void *unused)
{
	(void)unused;
	case_name = "setup";
#if OVE_EVENT_WAITERS_TARGET_CANARY
	check(false, "early failure survives all later native controls");
#endif
	check(control() == 0, "supervisor enters privileged");
#ifdef OVE_MPU_V1
	check((*(volatile uint32_t *)0xe000ed94 & 1) != 0, "actual MPU is enabled");
#endif
	*(volatile uint8_t *)0xe000e400 = 0xa0;
	*(volatile uint8_t *)0xe000e401 = 0x60;
	*(volatile uint32_t *)0xe000e280 = 3;
	*(volatile uint32_t *)0xe000e100 = 3;
	for (unsigned restricted = 0; restricted <= 1; ++restricted) {
#if OVE_NATIVE_TRACE
		/* The trace ring is privileged global storage; these controls do
		 * not grant it to restricted workers or exercise an invalid access. */
		if (restricted)
			break;
#endif
#ifndef OVE_MPU_V1
		if (restricted)
			break;
#endif
		separated(2, restricted);
		separated(3, restricted);
		one_signal(restricted);
		poll(restricted);
		for (unsigned selected = 0; selected < 3; ++selected) {
			retire(selected, restricted, false);
#if OVE_NATIVE_ABORT
			retire(selected, restricted, true);
#endif
		}
		suspended(restricted);
		hint(restricted);
		reuse(restricted);
		irq(restricted, false);
		irq(restricted, true);
		producer(restricted, restricted && !OVE_PRIVILEGED_PRODUCER_ONLY);
	}
	raw_mask(0, 0x40, 0);
	raw_mask(1, 0, 0);
	raw_mask(0, 0, 1);
	native_contexts();
	*(volatile uint32_t *)0xe000e180 = 3;
	*(volatile uint32_t *)0xe000e280 = 3;
	printf("freertos event waiters native: mpu=%u enforce=%u erratum=%u abort=%u trace=%u "
	       "producer_priv_only=%u heap=%u "
	       "ticks=%u cases=%u workers=%u headroom=%u checks=%u failures=%u\n",
#ifdef OVE_MPU_V1
	       1u,
#else
	       0u,
#endif
	       OVE_SYSCALL_ENFORCE, OVE_ERRATUM, OVE_NATIVE_ABORT, OVE_NATIVE_TRACE,
	       OVE_PRIVILEGED_PRODUCER_ONLY,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       OVE_NATIVE_TICK_BITS, cases, workers, minimum_headroom, checks, failures);
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
#ifdef OVE_MPU_V1
	if (((*(volatile uint32_t *)0xe000ed90 >> 8) & 0xff) != OVE_MPU_REGIONS)
		event_waiters_target_reject();
	const UBaseType_t priority = 2 | portPRIVILEGE_BIT;
#else
	const UBaseType_t priority = 2;
#endif
	TaskHandle_t task = xTaskCreateStatic(supervisor, "supervisor", 1024, NULL, priority,
					      supervisor_stack, &supervisor_tcb);
	if (!task)
		event_waiters_target_reject();
	vTaskStartScheduler();
	event_waiters_target_reject();
}
