/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/irq.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct state {
	unsigned control, p, b, f;
};
struct payload {
	struct state before, held, inner, after;
	atomic_uint phase, done, kernel_calls, system_calls, interrupted_user, bad_handler;
	unsigned blocked, restricted;
};
union shared_region {
	struct payload state;
	unsigned char bytes[512];
};
_Static_assert(sizeof(union shared_region) == 512, "reserve the full shared MPU grant");
static union shared_region shared __attribute__((aligned(512)));
#define user shared.state
static StaticTask_t worker_tcb, supervisor_tcb;
static StackType_t worker_stack[512] __attribute__((aligned(2048)));
static StackType_t supervisor_stack[1024];
static unsigned checks, failures, cases, workers, minimum_headroom = 2048;
static atomic_uint observer_calls;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(atomic_uint) == 4,
	       "native observers require lock-free word atomics");
#ifdef OVE_MPU_V1
#define SHARED_ATTRIBUTES                                           \
	(portMPU_REGION_READ_WRITE | portMPU_REGION_EXECUTE_NEVER | \
	 portMPU_REGION_CACHEABLE_BUFFERABLE)
_Static_assert((SHARED_ATTRIBUTES & (7u << 16)) == (7u << 16),
	       "shared atomics require the native Normal SRAM memory type");
#define NATIVE_MPU 1
#else
#define SHARED_ATTRIBUTES 0
#define NATIVE_MPU 0
#define OVE_MPU_REGIONS 0
#endif
#ifdef CONFIG_OVE_ZERO_HEAP
#define NATIVE_HEAP 0
#else
#define NATIVE_HEAP 1
#endif
/* The artifact gate reads these compiled constants before starting a guest. */
const uint32_t irq_profile[] __attribute__((section(".irq_profile"), used)) = {
	0x49525131,
	NATIVE_MPU,
	OVE_SYSCALL_ENFORCE,
	OVE_ALLOW_UNPRIVILEGED_CRITICAL,
	OVE_ALL_CALLERS_PRIVILEGED,
	NATIVE_HEAP,
	OVE_IRQ_TARGET_CANARY,
	OVE_ERRATUM,
	OVE_MPU_REGIONS,
	sizeof(shared),
	sizeof(worker_stack),
	sizeof(worker_tcb),
	SHARED_ATTRIBUTES,
	ATOMIC_INT_LOCK_FREE,
	configNUMBER_OF_CORES,
	configMAX_SYSCALL_INTERRUPT_PRIORITY,
	configPRIO_BITS,
};

static unsigned control(void)
{
	unsigned v;
	__asm volatile("mrs %0, control" : "=r"(v));
	return v;
}
static unsigned primask(void)
{
	unsigned v;
	__asm volatile("mrs %0, primask" : "=r"(v));
	return v;
}
static unsigned basepri(void)
{
	unsigned v;
	__asm volatile("mrs %0, basepri" : "=r"(v));
	return v;
}
static unsigned faultmask(void)
{
	unsigned v;
	__asm volatile("mrs %0, faultmask" : "=r"(v));
	return v;
}
#ifdef OVE_MPU_V1
static struct state caller_state(void) FREERTOS_SYSTEM_CALL __attribute__((noinline));
static void pend_kernel(void) FREERTOS_SYSTEM_CALL __attribute__((noinline));
#endif
static struct state caller_state(void)
{
	unsigned mode = control();
#ifdef OVE_MPU_V1
	unsigned exception;
	__asm volatile("mrs %0, ipsr" : "=r"(exception));
	/* CONTROL describes Thread privilege even while a handler is privileged. */
	BaseType_t before = exception ? pdTRUE : portIS_PRIVILEGED();
	if (before == pdFALSE) {
		portRAISE_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	struct state result = {mode, primask(), basepri(), faultmask()};
#ifdef OVE_MPU_V1
	if (before == pdFALSE) {
		portRESET_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	return result;
}
static void pend_kernel(void)
{
#ifdef OVE_MPU_V1
	BaseType_t before = portIS_PRIVILEGED();
	if (before == pdFALSE) {
		portRAISE_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
	*(volatile uint32_t *)0xe000e200 = 2;
	__asm volatile("dsb\n\tisb" : : : "memory");
#ifdef OVE_MPU_V1
	if (before == pdFALSE) {
		portRESET_PRIVILEGE();
		portMEMORY_BARRIER();
	}
#endif
}
static void masks(unsigned p, unsigned b, unsigned f)
{
	__asm volatile(
		"cpsid i\n\tmsr basepri, %0\n\tdsb\n\tisb\n\tmsr primask, %1\n\tmsr faultmask, %2\n\tisb"
		:
		: "r"(b), "r"(p), "r"(f)
		: "memory");
}
static bool same(struct state a, struct state b)
{
	return a.control == b.control && a.p == b.p && a.b == b.b && a.f == b.f;
}
static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", message);
	}
}
void irq_masks_target_reject(void)
{
	fputs("native IRQ fixture rejected invalid setup or completion\n", stdout);
	semihosting_exit(3);
}
void irq_masks_target_assert(unsigned line)
{
	printf("native IRQ fixture assertion: line=%u\n", line);
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	irq_masks_target_reject();
}
void UART0RX_IRQHandler(void)
{
	/* Above-threshold observer never uses the raw lock or kernel APIs. */
	atomic_fetch_add_explicit(&observer_calls, 1, memory_order_relaxed);
}
void UART0TX_IRQHandler(void)
{
	struct state before = caller_state();
	ove_irq_key_t first = ove_irq_lock();
	ove_irq_key_t second = ove_irq_lock();
	struct state held = caller_state();
	ove_irq_unlock(second);
	ove_irq_unlock(first);
	struct state after = caller_state();
	atomic_fetch_or_explicit(&user.bad_handler,
				 !same(before, after) || held.p || held.f || held.b != 128,
				 memory_order_relaxed);
	atomic_fetch_or_explicit(&user.interrupted_user, before.control & 1u, memory_order_relaxed);
	atomic_fetch_add_explicit(&user.kernel_calls, 1, memory_order_release);
}
void vApplicationTickHook(void)
{
	if (atomic_load_explicit(&user.phase, memory_order_acquire) != 2)
		return;
	atomic_store_explicit(&user.phase, 3, memory_order_relaxed);
	/* This is an eligible SysTick activation, even when CONTROL describes
	 * the interrupted restricted Thread. Neither route uses a task syscall. */
	struct state before = caller_state();
	ove_irq_key_t key = ove_irq_lock();
	ove_irq_unlock(key);
	struct state after = caller_state();
	atomic_fetch_or_explicit(&user.bad_handler, !same(before, after), memory_order_relaxed);
	atomic_fetch_add_explicit(&user.system_calls, 1, memory_order_relaxed);
	*(volatile uint32_t *)0xe000e200 = 2;
}
static void worker(void *unused)
{
	(void)unused;
	user.before = caller_state();
	ove_irq_key_t first = ove_irq_lock();
	user.held = caller_state();
	ove_irq_key_t second = ove_irq_lock();
	ove_irq_unlock(second);
	user.inner = caller_state();
	pend_kernel();
	user.blocked = atomic_load_explicit(&user.kernel_calls, memory_order_acquire) == 0;
	ove_irq_unlock(first);
	user.after = caller_state();
	atomic_store_explicit(&user.phase, 2, memory_order_release);
	/* Wait only after both keys have retired. The next real tick admits the
	 * system-handler control and pending eligible external interrupt. */
	unsigned turns = 0;
	while (atomic_load_explicit(&user.kernel_calls, memory_order_acquire) < 2 &&
	       turns++ < 10000000u)
		__asm volatile("nop");
	atomic_store_explicit(&user.done, 1, memory_order_release);
	vTaskSuspend(NULL);
	for (;;)
		__asm volatile("nop");
}
static void workers_control(bool restricted)
{
	++cases;
	++workers;
	memset(&shared, 0, sizeof(shared));
	atomic_init(&user.phase, 0);
	atomic_init(&user.done, 0);
	atomic_init(&user.kernel_calls, 0);
	atomic_init(&user.system_calls, 0);
	atomic_init(&user.interrupted_user, 0);
	atomic_init(&user.bad_handler, 0);
	user.restricted = restricted;
	TaskHandle_t handle = NULL;
#ifdef OVE_MPU_V1
	if (restricted) {
		TaskParameters_t parameters = {
			.pvTaskCode = worker,
			.pcName = "restricted",
			.usStackDepth = 512,
			.pvParameters = NULL,
			.uxPriority = 4,
			.puxStackBuffer = worker_stack,
			.xRegions = {{.pvBaseAddress = &shared,
				      .ulLengthInBytes = 512,
				      .ulParameters = SHARED_ATTRIBUTES}},
			.pxTaskBuffer = &worker_tcb,
		};
		if (xTaskCreateRestrictedStatic(&parameters, &handle) != pdPASS)
			irq_masks_target_reject();
	} else {
		handle = xTaskCreateStatic(worker, "privileged", 512, NULL, 4 | portPRIVILEGE_BIT,
					   worker_stack, &worker_tcb);
	}
#else
	handle = xTaskCreateStatic(worker, "ordinary", 512, NULL, 4, worker_stack, &worker_tcb);
#endif
	if (!handle)
		irq_masks_target_reject();
	if (!atomic_load_explicit(&user.done, memory_order_acquire) ||
	    eTaskGetState(handle) != eSuspended)
		irq_masks_target_reject();
	check(same(user.before, user.after), "public task round trip restores real masks and mode");
	check(user.before.p == 0 && user.before.b == 0 && user.before.f == 0,
	      "supported worker starts unmasked");
	check((user.before.control & 1u) == restricted, "actual worker privilege matches its role");
	check(user.held.control == user.before.control && user.held.p == 0 && user.held.b == 128 &&
		      user.held.f == 0,
	      "body retains native threshold with caller privilege and no extra masks");
	check(same(user.held, user.inner), "nested key restores the enclosing section");
	check(user.blocked && atomic_load_explicit(&user.kernel_calls, memory_order_acquire) == 2,
	      "eligible IRQ defers until release and completes both activations");
	check(atomic_load_explicit(&user.system_calls, memory_order_relaxed) == 1 &&
		      !atomic_load_explicit(&user.bad_handler, memory_order_relaxed),
	      "external and SysTick handlers restore mode and native state");
	check(atomic_load_explicit(&user.interrupted_user, memory_order_relaxed) == restricted,
	      "handler dispatch handles the interrupted Thread privilege correctly");
	unsigned headroom = uxTaskGetStackHighWaterMark(handle) * sizeof(StackType_t);
	if (headroom < minimum_headroom)
		minimum_headroom = headroom;
	check(headroom >= 512, "tested worker retains bounded stack headroom");
	/* All public keys are retired and the real worker is suspended before
	 * deleting it. Static backing remains live for the entire program. */
	vTaskDelete(handle);
}
static void masked_control(unsigned p, unsigned b, unsigned f)
{
	++cases;
	atomic_store_explicit(&observer_calls, 0, memory_order_relaxed);
	masks(p, b, f);
	struct state before = caller_state();
	*(volatile uint32_t *)0xe000e200 = 1;
	ove_irq_key_t key = ove_irq_lock();
	struct state held = caller_state();
	unsigned during = atomic_load_explicit(&observer_calls, memory_order_relaxed);
	ove_irq_unlock(key);
	struct state after = caller_state();
	unsigned admitted = atomic_load_explicit(&observer_calls, memory_order_relaxed);
	masks(0, 0, 0);
	for (unsigned turn = 0;
	     turn < 128 && !atomic_load_explicit(&observer_calls, memory_order_relaxed); ++turn)
		__asm volatile("nop");
	check(held.p == p && held.b == (b && b < 128 ? b : 128) && held.f == f && !during,
	      "wrapper entry preserves stronger masks and excludes the harmless observer");
	check(same(before, after) && !admitted,
	      "matching native release restores the exact incoming state");
	check(atomic_load_explicit(&observer_calls, memory_order_relaxed) == 1,
	      "explicit unmask admits the retained observer");
}
static void native_body_limit(void)
{
	++cases;
	atomic_store_explicit(&observer_calls, 0, memory_order_relaxed);
	masks(0, 64, 0);
	ove_irq_key_t key = ove_irq_lock();
	*(volatile uint32_t *)0xe000e200 = 1;
	taskENTER_CRITICAL();
	taskEXIT_CRITICAL();
	unsigned body_b = basepri(),
		 admitted = atomic_load_explicit(&observer_calls, memory_order_relaxed);
	ove_irq_unlock(key);
	unsigned restored_b = basepri();
	masks(0, 0, 0);
	check(body_b == 128 && admitted == 1,
	      "stock native body has the documented stronger-mask limitation");
	check(restored_b == 64, "wrapper boundary restores input after the native body");
}
static void supervisor(void *unused)
{
	(void)unused;
#ifdef OVE_MPU_V1
	if ((*(volatile uint32_t *)0xe000ed94 & 1u) == 0 || *(volatile uint8_t *)0xe000ed1f != 0)
		irq_masks_target_reject();
#endif
	if ((*(volatile uint32_t *)0xe000ed0c & 0x700u) != 0)
		irq_masks_target_reject();
	*(volatile uint8_t *)0xe000e400 = 64;
	*(volatile uint8_t *)0xe000e401 = 144;
	*(volatile uint32_t *)0xe000e280 = 3;
	*(volatile uint32_t *)0xe000e100 = 3;
	masked_control(1, 0, 0);
	masked_control(0, 64, 0);
	masked_control(0, 0, 1);
	native_body_limit();
	workers_control(false);
#if defined(OVE_MPU_V1) && !OVE_ALL_CALLERS_PRIVILEGED
	workers_control(true);
#endif
	*(volatile uint32_t *)0xe000e180 = 3;
	*(volatile uint32_t *)0xe000e280 = 3;
	printf("freertos irq native: mpu=%u enforce=%u allow=%u all_priv=%u heap=%u cases=%u workers=%u headroom=%u checks=%u failures=%u\n",
#ifdef OVE_MPU_V1
	       1u,
#else
	       0u,
#endif
	       OVE_SYSCALL_ENFORCE, OVE_ALLOW_UNPRIVILEGED_CRITICAL, OVE_ALL_CALLERS_PRIVILEGED,
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
#if OVE_IRQ_TARGET_CANARY
	check(false, "early failure survives later native IRQ controls");
#endif
	++cases;
	struct state before = caller_state();
	ove_irq_key_t key = ove_irq_lock();
	ove_irq_unlock(key);
	struct state after = caller_state();
	masks(0, 0, 0);
	check(same(before, after),
	      "balanced prestart lock preserves masks and native startup nesting");
#ifdef OVE_MPU_V1
	if (((*(volatile uint32_t *)0xe000ed90 >> 8) & 255u) != OVE_MPU_REGIONS)
		irq_masks_target_reject();
	UBaseType_t priority = 2 | portPRIVILEGE_BIT;
#else
	UBaseType_t priority = 2;
#endif
	if (!xTaskCreateStatic(supervisor, "supervisor", 1024, NULL, priority, supervisor_stack,
			       &supervisor_tcb))
		irq_masks_target_reject();
	vTaskStartScheduler();
	irq_masks_target_reject();
}
