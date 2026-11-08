/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
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
	atomic_uint done;
	unsigned restricted, producer, all, spurious, entry, exit, boundaries, bad_boundary;
	bool held;
	int result;
};
struct user_payload {
	ove_mutex_storage_t mutex_storage;
	ove_condvar_storage_t cv_storage;
	ove_mutex_t mutex;
	ove_condvar_t cv;
	struct job jobs[3];
};
union user_region {
	struct user_payload state;
	unsigned char region[512];
};
_Static_assert(sizeof(union user_region) == 512, "reserve the whole shared MPU grant");
static union user_region shared __attribute__((aligned(512)));
#define user shared.state
static StaticTask_t worker_tcbs[3], supervisor_tcb;
static StackType_t worker_stacks[3][512] __attribute__((aligned(2048)));
static StackType_t supervisor_stack[1024];
static TaskHandle_t handles[3];
static unsigned checks, failures, cases, workers, minimum_headroom = 2048;
static const char *case_name;
static volatile unsigned observer_calls;

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
struct caller_state {
	unsigned mode, primask, basepri, faultmask;
};
#ifdef OVE_MPU_V1
static struct caller_state caller_state(void) FREERTOS_SYSTEM_CALL __attribute__((noinline));
#endif
static struct caller_state caller_state(void)
{
	unsigned mode = control();
#ifdef OVE_MPU_V1
	BaseType_t was_privileged = portIS_PRIVILEGED();
	if (was_privileged == pdFALSE) {
		portRAISE_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	/* Restricted MRS of these registers would read zero. Observe actual
	 * masks after legitimate promotion, without a native critical entry. */
	struct caller_state state = {mode, primask(), basepri(), faultmask()};
#ifdef OVE_MPU_V1
	if (was_privileged == pdFALSE) {
		portMEMORY_BARRIER();
		portRESET_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	return state;
}
static void masks(unsigned p, unsigned b, unsigned f)
{
	__asm volatile("cpsid i\n\tmsr basepri, %0\n\tdsb\n\tisb\n\tmsr primask, %1\n\t"
		       "msr faultmask, %2\n\tisb"
		       :
		       : "r"(b), "r"(p), "r"(f)
		       : "memory");
}
void cv_context_target_reject(void)
{
	fputs("native CV fixture rejected an invalid setup or completion state\n", stdout);
	semihosting_exit(3);
}
void cv_context_target_assert(unsigned line)
{
	printf("native CV fixture assertion: line=%u case=%s\n", line,
	       case_name ? case_name : "startup");
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	cv_context_target_reject();
}
static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s: %s\n", case_name, message);
	}
}
static void observe_boundary(void)
{
	struct job *job = pvTaskGetThreadLocalStoragePointer(NULL, 0);
	if (!job)
		return;
	++job->boundaries;
	struct caller_state state = caller_state();
	if (state.mode != job->restricted || state.primask || state.basepri || state.faultmask)
		++job->bad_boundary;
}
#ifdef OVE_MPU_V1
#define NATIVE(name) MPU_##name
#define REAL(name) __real_MPU_##name
#define WRAP(name) __wrap_MPU_##name
#else
#define NATIVE(name) name
#define REAL(name) __real_##name
#define WRAP(name) __wrap_##name
#endif
uint32_t REAL(ulTaskGenericNotifyTake)(UBaseType_t, BaseType_t, TickType_t);
uint32_t WRAP(ulTaskGenericNotifyTake)(UBaseType_t index, BaseType_t clear, TickType_t ticks)
{
	observe_boundary();
	return REAL(ulTaskGenericNotifyTake)(index, clear, ticks);
}
BaseType_t REAL(xQueueGenericSend)(QueueHandle_t, const void *, TickType_t, BaseType_t);
BaseType_t WRAP(xQueueGenericSend)(QueueHandle_t queue, const void *item, TickType_t ticks,
				   BaseType_t position)
{
	observe_boundary();
	return REAL(xQueueGenericSend)(queue, item, ticks, position);
}
BaseType_t REAL(xQueueSemaphoreTake)(QueueHandle_t, TickType_t);
BaseType_t WRAP(xQueueSemaphoreTake)(QueueHandle_t queue, TickType_t ticks)
{
	observe_boundary();
	return REAL(xQueueSemaphoreTake)(queue, ticks);
}
static void worker(void *data)
{
	struct job *job = data;
	vTaskSetThreadLocalStoragePointer(NULL, 0, job);
	job->entry = control();
	if (job->producer) {
		if (job->all)
			ove_condvar_broadcast(user.cv);
		else
			ove_condvar_signal(user.cv);
	} else {
		if (ove_mutex_lock(user.mutex, OVE_WAIT_FOREVER))
			cv_context_target_reject();
		if (job->spurious)
			xTaskNotifyGive(xTaskGetCurrentTaskHandle());
		job->result =
			ove_condvar_wait_checked(user.cv, user.mutex, job->timeout, &job->held);
		observe_boundary();
		/* Check actual native ownership before any unlock on an error path. */
		if (!job->held ||
		    xSemaphoreGetMutexHolder(user.mutex->sem) != xTaskGetCurrentTaskHandle())
			cv_context_target_reject();
		ove_mutex_unlock(user.mutex);
	}
	observe_boundary();
	job->exit = control();
	atomic_store_explicit(&job->done, 1, memory_order_release);
	vTaskSuspend(NULL);
	cv_context_target_reject();
}
static void begin(const char *name)
{
	case_name = name;
	memset(&shared, 0, sizeof(shared));
	memset(handles, 0, sizeof(handles));
	if (ove_mutex_init(&user.mutex, &user.mutex_storage) ||
	    ove_condvar_init(&user.cv, &user.cv_storage))
		cv_context_target_reject();
}
static void start(unsigned index, bool restricted, uint64_t timeout, bool producer, bool all,
		  bool spurious)
{
#if OVE_ALL_CALLERS_PRIVILEGED || OVE_NATIVE_TRACE
	restricted = false;
#endif
	struct job *job = &user.jobs[index];
	*job = (struct job){.timeout = timeout,
			    .restricted = restricted,
			    .producer = producer,
			    .all = all,
			    .spurious = spurious,
			    .result = OVE_ERR_BUSY};
	atomic_init(&job->done, 0);
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
			.xRegions = {{.pvBaseAddress = &shared,
				      .ulLengthInBytes = 512,
				      .ulParameters = portMPU_REGION_READ_WRITE |
						      portMPU_REGION_EXECUTE_NEVER}},
			.pxTaskBuffer = &worker_tcbs[index],
		};
		if (xTaskCreateRestrictedStatic(&parameters, &task) != pdPASS)
			cv_context_target_reject();
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
		cv_context_target_reject();
	handles[index] = task;
	++workers;
	/* The higher-priority worker runs through publication, mutex release and
	 * blocking before the supervisor can continue. State is observed natively. */
	check(eTaskGetState(task) ==
		      ((producer || timeout == 0 || spurious) ? eSuspended : eBlocked),
	      "native worker reaches its public waiting/completed boundary");
}
static bool done(unsigned index)
{
	return atomic_load_explicit(&user.jobs[index].done, memory_order_acquire) != 0;
}
static bool await_done(unsigned index, unsigned ticks)
{
	for (unsigned i = 0; i < ticks && !done(index); ++i)
		vTaskDelay(1);
	return done(index);
}
static void finish(unsigned count)
{
	/* Record semantic failures before unconditional valid cleanup. */
	for (unsigned turn = 0; turn < count + 2; ++turn) {
		ove_condvar_broadcast(user.cv);
		for (unsigned i = 0; i < count; ++i)
			if (!done(i))
				xTaskNotifyGive(handles[i]);
		vTaskDelay(1);
	}
	for (unsigned i = 0; i < count; ++i) {
		struct job *job = &user.jobs[i];
		if (!done(i) || eTaskGetState(handles[i]) != eSuspended)
			cv_context_target_reject();
		check(job->entry == job->restricted && job->exit == job->restricted &&
			      !job->bad_boundary,
		      "caller mode and masks hold at every native boundary and public return");
		check(job->producer || (job->held && job->boundaries >= 4),
		      "wait returns with one owned mutex level");
		unsigned headroom = uxTaskGetStackHighWaterMark(handles[i]) * sizeof(StackType_t);
		if (headroom < minimum_headroom)
			minimum_headroom = headroom;
		check(headroom >= 256, "instrumented worker retains bounded stack headroom");
		vTaskDelete(handles[i]);
	}
	ove_condvar_deinit(user.cv);
	ove_mutex_deinit(user.mutex);
#if OVE_NATIVE_TRACE
	struct ove_trace_record records[8];
	unsigned posts = 0;
	size_t available;
	while ((available = ove_trace_ring_drain(records, 8)) != 0)
		for (size_t i = 0; i < available; ++i)
			if (records[i].kind == OVE_TRACE_KIND_MARK &&
			    records[i].code == ((OVE_TRACE_PRIM_CV << 4) | OVE_TRACE_ACT_POST))
				++posts;
	check(posts && !ove_trace_ring_dropped_get(), "actual privileged trace retains CV posts");
#endif
	check(!control() && !primask() && !basepri() && !faultmask(),
	      "supervisor cleanup restores caller state");
	++cases;
}
static void waits(bool restricted)
{
	for (unsigned kind = 0; kind < 3; ++kind) {
		begin("timeout and spurious");
		start(0, restricted,
		      kind == 0	  ? 0
		      : kind == 1 ? 2000000
				  : OVE_WAIT_FOREVER,
		      false, false, kind == 2);
		check(await_done(0, 20) &&
			      user.jobs[0].result == (kind == 2 ? OVE_OK : OVE_ERR_TIMEOUT),
		      "native zero, finite and genuine spurious results are preserved");
		finish(1);
	}
}
static void notify(bool restricted, bool all, bool held)
{
	begin("foreign notification");
	start(0, restricted, OVE_WAIT_FOREVER, false, false, false);
	start(1, restricted, OVE_WAIT_FOREVER, false, false, false);
	if (ove_mutex_lock(user.mutex, OVE_WAIT_FOREVER))
		cv_context_target_reject();
	if (!held)
		ove_mutex_unlock(user.mutex);
	start(2, restricted, 0, true, all, false);
	if (held) {
		check(!done(0) && !done(1), "notifier-held mutex excludes waiter completion");
		ove_mutex_unlock(user.mutex);
	}
	check(await_done(1, 20) && user.jobs[1].result == OVE_OK,
	      "distinct producer wakes the newest live waiter");
	if (all)
		check(await_done(0, 20) && user.jobs[0].result == OVE_OK,
		      "broadcast wakes both foreign stacks");
	else
		check(!done(0), "signal leaves the other invocation registered");
	finish(3);
}
static void retire(bool restricted)
{
	begin("foreign retirement");
	start(0, restricted, OVE_WAIT_FOREVER, false, false, false);
	start(1, restricted, OVE_WAIT_FOREVER, false, false, false);
	/* Both publications are established before this legitimate spurious
	 * native wake. The newer head stays live until the later CV signal. */
	xTaskNotifyGive(handles[0]);
	check(await_done(0, 20) && user.jobs[0].result == OVE_OK && !done(1),
	      "older waiter retires through the newer live private stack");
	ove_condvar_signal(user.cv);
	check(await_done(1, 20) && user.jobs[1].result == OVE_OK,
	      "foreign head survives older retirement");
	finish(2);
}
void UART0TX_IRQHandler(void)
{
	++observer_calls;
}
static void masked(unsigned p, unsigned b, unsigned f)
{
	begin("privileged mask preservation");
	start(0, false, OVE_WAIT_FOREVER, false, false, false);
	observer_calls = 0;
	masks(p, b, f);
	*(volatile uint32_t *)0xe000e200 = 2;
	__asm volatile("dsb\n\tisb" : : : "memory");
	ove_condvar_broadcast(user.cv);
	unsigned after_p = primask(), after_b = basepri(), after_f = faultmask(),
		 admitted = observer_calls;
	masks(0, 0, 0);
	__asm volatile("dsb\n\tisb" : : : "memory");
	check(after_p == p && after_b == b && after_f == f && !admitted,
	      "nonblocking privileged producer continuously preserves incoming masks");
	check(observer_calls == 1 && await_done(0, 20),
	      "explicit unmask admits the harmless observer and waiter");
	finish(1);
}
static void supervisor(void *unused)
{
	(void)unused;
	case_name = "setup";
#ifdef OVE_MPU_V1
	if ((*(volatile uint32_t *)0xe000ed94 & 1u) == 0)
		cv_context_target_reject();
	check(true, "native MPU is enabled before any restricted worker starts");
#endif
#if OVE_CV_CONTEXT_TARGET_CANARY
	check(false, "early failure survives later native controls");
#endif
	*(volatile uint8_t *)0xe000e401 = 0x60;
	*(volatile uint32_t *)0xe000e280 = 2;
	*(volatile uint32_t *)0xe000e100 = 2;
	for (unsigned restricted = 0; restricted <= 1; ++restricted) {
#if !defined(OVE_MPU_V1) || OVE_ALL_CALLERS_PRIVILEGED || OVE_NATIVE_TRACE
		if (restricted)
			break;
#endif
		waits(restricted);
		for (unsigned all = 0; all < 2; ++all)
			for (unsigned held = 0; held < 2; ++held)
				notify(restricted, all, held);
		retire(restricted);
	}
	masked(1, 0, 0);
	masked(0, 0x40, 0);
	masked(0, 0, 1);
	*(volatile uint32_t *)0xe000e180 = 2;
	*(volatile uint32_t *)0xe000e280 = 2;
	printf("freertos cv native: mpu=%u enforce=%u allow=%u trace=%u all_priv=%u heap=%u "
	       "cases=%u workers=%u headroom=%u checks=%u failures=%u\n",
#ifdef OVE_MPU_V1
	       1u,
#else
	       0u,
#endif
	       OVE_SYSCALL_ENFORCE, OVE_ALLOW_UNPRIVILEGED_CRITICAL, OVE_NATIVE_TRACE,
	       OVE_ALL_CALLERS_PRIVILEGED,
#ifdef CONFIG_OVE_ZERO_HEAP
	       0u,
#else
	       1u,
#endif
	       cases, workers, minimum_headroom, checks, failures);
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
#ifdef OVE_MPU_V1
	if (((*(volatile uint32_t *)0xe000ed90 >> 8) & 0xff) != OVE_MPU_REGIONS)
		cv_context_target_reject();
	UBaseType_t priority = 2 | portPRIVILEGE_BIT;
#else
	UBaseType_t priority = 2;
#endif
	if (!xTaskCreateStatic(supervisor, "supervisor", 1024, NULL, priority, supervisor_stack,
			       &supervisor_tcb))
		cv_context_target_reject();
	vTaskStartScheduler();
	cv_context_target_reject();
}
