/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/sync.h"
#include "ove/thread.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "framework/semihosting_exit.h"

#define STACK_BYTES 4096u
#define STACK_WORDS (STACK_BYTES / sizeof(StackType_t))
_Static_assert(STACK_WORDS >= configMINIMAL_STACK_SIZE, "stack covers native minimum");
_Static_assert(configUSE_PREEMPTION == 1 && configMAX_PRIORITIES == 32,
	       "control requires distinct preemptive priorities");
_Static_assert(configUSE_TIMERS == 0 && configTICK_RATE_HZ == 1000,
	       "control requires the qualified tick without a timer daemon");

#if configSUPPORT_DYNAMIC_ALLOCATION
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));
#endif
static StaticTask_t controller_tcb;
static StackType_t controller_stack[STACK_WORDS] __attribute__((aligned(8)));
static ove_thread_storage_t worker_storage;
static uint8_t worker_stack[STACK_BYTES] __attribute__((aligned(8)));
static ove_event_storage_t event_storage;
static ove_event_t event;
static StaticSemaphore_t ready_storage, done_storage;
static SemaphoreHandle_t ready, done;
static unsigned failures, scenarios, joins, generation, wait_returns;
static unsigned minimum_worker_headroom = STACK_BYTES;
static struct {
	unsigned generation, finished;
	int result;
} job;
static unsigned irq_armed, irq_calls, irq_ipsr, irq_mask_before, irq_mask_after;

static void text(const char *message)
{
	register unsigned operation __asm("r0") = 4; /* SYS_WRITE0 */
	register const char *argument __asm("r1") = message;
	__asm volatile("bkpt 0xab" : "+r"(operation) : "r"(argument) : "memory");
}

static void number(unsigned value)
{
	char digits[11];
	unsigned at = sizeof(digits) - 1;
	digits[at] = '\0';
	do {
		digits[--at] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	text(&digits[at]);
}

static void field(const char *name, unsigned value)
{
	text(name);
	number(value);
}

static unsigned ipsr(void)
{
	unsigned value;
	__asm volatile("mrs %0, ipsr" : "=r"(value));
	return value;
}

static unsigned mask(void)
{
	unsigned value;
	__asm volatile("mrs %0, basepri" : "=r"(value));
	return value;
}

static void barriers(void)
{
	__asm volatile("dsb\nisb" ::: "memory");
}

void event_target_reject(void)
{
	text("rejected: native Event control\n");
	semihosting_exit(2);
}

static void check(bool condition, const char *message)
{
	if (!condition) {
		++failures;
		text("failed: ");
		text(message);
		text("\n");
	}
}

static void take(SemaphoreHandle_t gate)
{
	if (xSemaphoreTake(gate, pdMS_TO_TICKS(1000)) != pdTRUE)
		event_target_reject();
}

static void give(SemaphoreHandle_t gate)
{
	if (xSemaphoreGive(gate) != pdTRUE)
		event_target_reject();
}

static int wait_event(uint64_t timeout)
{
	int status = ove_event_wait(event, timeout);
	__atomic_add_fetch(&wait_returns, 1u, __ATOMIC_RELAXED);
	return status;
}

void SysTick_Handler(void)
{
	extern void xPortSysTickHandler(void);
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
		xPortSysTickHandler();
}

void UART0RX_IRQHandler(void)
{
	if (!__atomic_load_n(&irq_armed, __ATOMIC_ACQUIRE))
		event_target_reject();
	irq_ipsr = ipsr();
	irq_mask_before = mask();
	ove_event_signal_from_isr(event);
	irq_mask_after = mask();
	__atomic_add_fetch(&irq_calls, 1u, __ATOMIC_RELEASE);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	event_target_reject();
}
void HardFault_Handler(void)
{
	event_target_reject();
}
void MemManage_Handler(void)
{
	event_target_reject();
}
void BusFault_Handler(void)
{
	event_target_reject();
}
void UsageFault_Handler(void)
{
	event_target_reject();
}
void NMI_Handler(void)
{
	event_target_reject();
}

static void worker(void *argument)
{
	(void)argument;
	unsigned current = __atomic_load_n(&job.generation, __ATOMIC_ACQUIRE);
	give(ready);
	int result = wait_event(1000000000);
	__atomic_store_n(&job.result, result, __ATOMIC_RELEASE);
	__atomic_store_n(&job.finished, current, __ATOMIC_RELEASE);
	give(done);
}

enum signal_kind { TASK_SIGNAL, ISR_TASK_SIGNAL, IRQ_SIGNAL };

static void wake_worker(const char *name, enum signal_kind kind, bool nested)
{
	if (uxSemaphoreGetCount(ready) || uxSemaphoreGetCount(done) || event->waiters)
		event_target_reject();
	++generation;
	__atomic_store_n(&job.generation, generation, __ATOMIC_RELEASE);
	__atomic_store_n(&job.finished, 0u, __ATOMIC_RELEASE);
	ove_thread_t thread = NULL;
	if (ove_thread_init(&thread, &worker_storage, "event-worker", worker, NULL,
			    OVE_PRIO_ABOVE_NORMAL, sizeof(worker_stack), worker_stack))
		event_target_reject();
	take(ready);
	/* The higher-priority worker runs from ready directly into its wait.
	 * No other worker exists, and this is its only blocking path. */
	if (ove_thread_get_state(thread) != OVE_THREAD_STATE_BLOCKED)
		event_target_reject();
	check(ipsr() == 0 && mask() == 0, "controller context before signal");
	if (nested)
		taskENTER_CRITICAL();
	unsigned before = mask();
	if (kind == TASK_SIGNAL) {
		ove_event_signal(event);
	} else if (kind == ISR_TASK_SIGNAL) {
		ove_event_signal_from_isr(event);
	} else {
		__atomic_store_n(&irq_armed, 1u, __ATOMIC_RELEASE);
		*(volatile uint32_t *)0xe000e200 = 1u; /* NVIC pending IRQ0 */
		barriers();
	}
	unsigned after = mask();
	check(before == after, "signal restores the native caller mask");
	if (nested) {
		check(before == configMAX_SYSCALL_INTERRUPT_PRIORITY &&
			      __atomic_load_n(&job.finished, __ATOMIC_ACQUIRE) == 0,
		      "outer native mask defers the worker");
		taskEXIT_CRITICAL();
	}
	take(done);
	if (ove_thread_get_state(thread) != OVE_THREAD_STATE_TERMINATED)
		event_target_reject();
	check(__atomic_load_n(&job.result, __ATOMIC_ACQUIRE) == OVE_OK &&
		      __atomic_load_n(&job.finished, __ATOMIC_ACQUIRE) == generation,
	      "worker result and generation");
	size_t headroom = 0;
	if (ove_thread_get_stack_headroom(thread, &headroom))
		event_target_reject();
	check(headroom >= 512, "worker stack headroom");
	if (headroom < minimum_worker_headroom)
		minimum_worker_headroom = headroom;
	if (kind == IRQ_SIGNAL) {
		*(volatile uint32_t *)0xe000e180 = 1u; /* disable IRQ0 */
		*(volatile uint32_t *)0xe000e280 = 1u; /* clear pending IRQ0 */
		barriers();
		__atomic_store_n(&irq_armed, 0u, __ATOMIC_RELEASE);
		check(__atomic_load_n(&irq_calls, __ATOMIC_ACQUIRE) == 1 && irq_ipsr == 16 &&
			      irq_mask_before == 0 && irq_mask_after == 0 && ipsr() == 0,
		      "genuine IRQ completes before retirement");
	}
	/* Completion is already established; do not enter an unfinished join. */
	if (ove_thread_deinit(thread))
		event_target_reject();
	thread = NULL;
	++joins;
	check(!event->waiters && mask() == 0, "waiter retired before backing reuse");
	text("observed: ");
	text(name);
	field(" before=", before);
	field(" after=", after);
	field(" generation=", generation);
	text(" rc=0 joined=1\n");
	++scenarios;
}

static void controller(void *argument)
{
	(void)argument;
#ifdef OVE_EVENT_TARGET_CANARY
	check(false, "explicit early failure canary");
#endif
	ready = xSemaphoreCreateCountingStatic(1, 0, &ready_storage);
	done = xSemaphoreCreateCountingStatic(1, 0, &done_storage);
	if (!ready || !done || ove_event_init(&event, &event_storage))
		event_target_reject();
	ove_event_signal(event);
	ove_event_signal(event);
	check(wait_event(0) == OVE_OK && wait_event(0) == OVE_ERR_TIMEOUT,
	      "preposted task signals coalesce");
	text("observed: presignal consumed=1 timeout=1\n");
	++scenarios;
	ove_event_signal_from_isr(event);
	check(mask() == 0 && wait_event(0) == OVE_OK && wait_event(0) == OVE_ERR_TIMEOUT,
	      "preposted FromISR task signal");
	text("observed: isr-presignal consumed=1 timeout=1 mask=0\n");
	++scenarios;
	TickType_t began = xTaskGetTickCount();
	int status = wait_event(10000000);
	TickType_t elapsed = xTaskGetTickCount() - began;
	check(status == OVE_ERR_TIMEOUT && elapsed >= 10 && elapsed <= 1000 && !event->waiters,
	      "finite timeout and tick progress");
	text("observed: timeout status=TIMEOUT ticks_in_range=1 retired=1\n");
	++scenarios;
	wake_worker("task", TASK_SIGNAL, false);
	wake_worker("task-masked", TASK_SIGNAL, true);
	wake_worker("isr-task", ISR_TASK_SIGNAL, false);
	wake_worker("isr-task-masked", ISR_TASK_SIGNAL, true);
	*(volatile uint32_t *)0xe000e180 = 1u;
	*(volatile uint32_t *)0xe000e280 = 1u;
	*(volatile uint8_t *)0xe000e400 = configMAX_SYSCALL_INTERRUPT_PRIORITY;
	barriers();
	if (*(volatile uint8_t *)0xe000e400 != configMAX_SYSCALL_INTERRUPT_PRIORITY)
		event_target_reject();
	*(volatile uint32_t *)0xe000e100 = 1u; /* enable IRQ0 */
	barriers();
	wake_worker("irq", IRQ_SIGNAL, false);
	wake_worker("reuse", TASK_SIGNAL, false);
	unsigned controller_headroom = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
	check(controller_headroom >= 512 && minimum_worker_headroom >= 512,
	      "application stack headroom");
	check(!event->waiters && !__atomic_load_n(&irq_armed, __ATOMIC_ACQUIRE),
	      "Event and IRQ quiescent before cleanup");
	ove_event_deinit(event);
	vSemaphoreDelete(ready);
	vSemaphoreDelete(done);
	field("native Event: scenarios=", scenarios);
	field(" returns=", __atomic_load_n(&wait_returns, __ATOMIC_RELAXED));
	field(" joins=", joins);
	field(" generations=", generation);
	field(" irq_calls=", __atomic_load_n(&irq_calls, __ATOMIC_ACQUIRE));
	field(" irq_ipsr=", irq_ipsr);
	field(" irq_mask=", irq_mask_before);
	field(",", irq_mask_after);
	field(" stack_headroom=", controller_headroom >= 512 && minimum_worker_headroom >= 512);
	text(" failures=");
	number(failures);
	text("\n");
	semihosting_exit(failures != 0);
}

int main(void)
{
	if (SystemCoreClock != 25000000 || configMINIMAL_STACK_SIZE < 512)
		event_target_reject();
	*(volatile uint32_t *)0xe000ed0c = 0x05fa0000u; /* AIRCR: priority grouping0 */
	barriers();
	if ((*(volatile uint32_t *)0xe000ed0c & 0x700u) != 0)
		event_target_reject();
	if (!xTaskCreateStatic(controller, "event-controller", STACK_WORDS, NULL, OVE_PRIO_LOW,
			       controller_stack, &controller_tcb))
		event_target_reject();
	vTaskStartScheduler();
	event_target_reject();
	return 2;
}
