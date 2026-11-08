/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.h"
#include "ove_freertos_tick.h"
#include "framework/semihosting_exit.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>

static StaticTask_t consumer_tcb, controller_tcb;
static StackType_t consumer_stack[1024], controller_stack[1024];
static TaskHandle_t consumer_task, controller_task;
static ove_eventgroup_storage_t backing;
static ove_eventgroup_t group;
static atomic_uint armed, tick_done, posted, notify_isr, notify_task, early, delivered;
static atomic_uint tick_ipsr, tick_mask, daemon_ok, fences;
static unsigned checks, failures;
static const char *failure_names[32];
static uint32_t pending_bits = 1;
static bool pending_clear;

static uint32_t ipsr(void)
{
	uint32_t value;
	__asm volatile("mrs %0, ipsr" : "=r"(value));
	return value;
}
static uint32_t basepri(void)
{
	uint32_t value;
	__asm volatile("mrs %0, basepri" : "=r"(value));
	return value;
}
static uint32_t primask(void)
{
	uint32_t value;
	__asm volatile("mrs %0, primask" : "=r"(value));
	return value;
}
static void check(bool ok, const char *name)
{
	++checks;
	if (!ok) {
		if (failures < 32)
			failure_names[failures] = name;
		++failures;
	}
}
void eventgroup_target_reject(void)
{
	semihosting_exit(3);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	eventgroup_target_reject();
}
static void tick(void)
{
	if (!atomic_exchange_explicit(&armed, 0, memory_order_acquire))
		return;
	atomic_store(&tick_ipsr, ipsr());
	atomic_store(&tick_mask, basepri());
	uint32_t result =
		pending_clear ? (uint32_t)ove_eventgroup_clear_bits_from_isr(group, pending_bits)
			      : ove_eventgroup_set_bits_from_isr(group, pending_bits);
	atomic_store(&posted, result);
	atomic_store_explicit(&tick_done, 1, memory_order_release);
}
static void notify(void *data)
{
	TaskHandle_t recipient = data;
	if (ipsr()) {
		atomic_fetch_add(&notify_isr, 1);
		BaseType_t wake = pdFALSE;
		vTaskNotifyGiveFromISR(recipient, &wake);
		portYIELD_FROM_ISR(wake);
	} else {
		atomic_fetch_add(&notify_task, 1);
		atomic_store(&daemon_ok,
			     xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle() &&
				     !basepri() && !primask());
		xTaskNotifyGive(recipient);
	}
}
static void consumer(void *unused)
{
	(void)unused;
	if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) {
		ove_eventbits_t bits = 0;
		int rc = ove_eventgroup_wait_bits(group, 1, 0, 0, &bits);
		if (rc == OVE_OK && (bits & 1))
			atomic_store(&delivered, 1);
		else {
			atomic_store(&early, 1);
			if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30))) {
				rc = ove_eventgroup_wait_bits(group, 1, 0, 0, &bits);
				atomic_store(&delivered, rc == OVE_OK && (bits & 1));
			}
		}
	}
	xTaskNotifyGive(controller_task);
	vTaskSuspend(NULL);
}
static void fence(void *data, uint32_t unused)
{
	(void)unused;
	atomic_fetch_add(&fences, 1);
	xTaskNotifyGive(data);
}
static bool timer_fence(void)
{
	return xTimerPendFunctionCall(fence, controller_task, 0, pdMS_TO_TICKS(100)) == pdPASS &&
	       ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) != 0;
}
#if OVE_DEFERRED_LIFETIME
static bool pulse_locked(uint32_t bits, bool clear)
{
	pending_bits = bits;
	pending_clear = clear;
	atomic_store(&tick_done, 0);
	atomic_store_explicit(&armed, 1, memory_order_release);
	TickType_t start = xTaskGetTickCount();
	while (!atomic_load_explicit(&tick_done, memory_order_acquire))
		if ((TickType_t)(xTaskGetTickCount() - start) > 10)
			return false;
	return true;
}
#endif
static void lifetime(void)
{
#if OVE_DEFERRED_LIFETIME
	check(ove_eventgroup_init(&group, &backing) == OVE_OK, "reuse owner initializes");
	check(ove_freertos_tick_subscribe(tick) == 0, "lifetime gate owns the tick subscriber");
	vTaskSuspendAll();
	check(pulse_locked(1, false) && atomic_load(&posted) == 1,
	      "genuine ISR admits work while daemon is suspended");
	ove_freertos_tick_unsubscribe(tick);
	ove_eventgroup_deinit(group);
	check(ove_eventgroup_init(&group, &backing) == OVE_OK,
	      "caller-owned backing reopens before canceled dispatch");
	(void)ove_eventgroup_set_bits(group, 2);
	check(ove_freertos_tick_subscribe(tick) == 0, "new generation owns the tick subscriber");
	check(pulse_locked(4, false) && atomic_load(&posted) == 4,
	      "new generation admits independent deferred work");
	ove_freertos_tick_unsubscribe(tick);
	(void)xTaskResumeAll();
	check(timer_fence() && ove_eventgroup_get_bits(group) == 6,
	      "canceled old work cannot modify reused backing");
	check(ove_freertos_tick_subscribe(tick) == 0, "clear gate owns the tick subscriber");
	vTaskSuspendAll();
	check(pulse_locked(4, true) && atomic_load(&posted) == OVE_OK,
	      "genuine ISR clear preserves admission status");
	ove_freertos_tick_unsubscribe(tick);
	(void)xTaskResumeAll();
	check(timer_fence() && ove_eventgroup_get_bits(group) == 2,
	      "deferred clear publishes before fence");
	ove_eventgroup_deinit(group);
#ifndef CONFIG_OVE_ZERO_HEAP
	check(ove_eventgroup_create(&group) == OVE_OK, "heap owner initializes");
	check(ove_freertos_tick_subscribe(tick) == 0, "heap retirement owns the tick subscriber");
	vTaskSuspendAll();
	check(pulse_locked(1, false) && atomic_load(&posted) == 1,
	      "heap owner admits deferred work");
	ove_freertos_tick_unsubscribe(tick);
	ove_eventgroup_destroy(group);
	group = NULL;
	(void)xTaskResumeAll();
	check(timer_fence(), "daemon safely consumes canceled work after immediate heap free");
#endif
#endif
}
static void controller(void *unused)
{
	(void)unused;
#if OVE_DEFERRED_CANARY
	check(false, "early failure survives later controls");
#endif
	check(ove_freertos_tick_subscribe(tick) == 0,
	      "wake gate owns the production tick subscriber");
	atomic_store_explicit(&armed, 1, memory_order_release);
	bool done = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(300)) != 0;
	ove_freertos_tick_unsubscribe(tick);
	check(done, "consumer reports a bounded delivery result");
	check(atomic_load(&tick_ipsr) == 15 &&
		      atomic_load(&tick_mask) == configMAX_SYSCALL_INTERRUPT_PRIORITY,
	      "native SysTick uses the configured kernel-aware interrupt mask");
	check(atomic_load(&posted) == 1, "ISR set reports its accepted mask");
	check(atomic_load(&delivered) == 1 && atomic_load(&early) == 0,
	      "publication supplies a wake that satisfies the higher-priority consumer");
	check(atomic_load(&notify_isr) == 0 && atomic_load(&notify_task) == 1 &&
		      atomic_load(&daemon_ok) == 1,
	      "notification follows publication in the unmasked timer daemon");
	check(timer_fence() && ove_eventgroup_get_bits(group) == 1,
	      "native publication and all queued work complete before teardown");
	check(ove_eventgroup_set_notify(group, NULL, NULL) == OVE_OK,
	      "callback retires after daemon fence");
	ove_eventgroup_deinit(group);
	vTaskDelete(consumer_task);
	lifetime();
	for (unsigned i = 0; i < failures && i < 32; ++i)
		printf("FAIL: %s\n", failure_names[i]);
	printf("eventgroup deferred target: checks=%u failures=%u ipsr=%u early=%u delivered=%u fences=%u\n",
	       checks, failures, atomic_load(&tick_ipsr), atomic_load(&early),
	       atomic_load(&delivered), atomic_load(&fences));
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
	if (ove_eventgroup_init(&group, &backing) != OVE_OK)
		semihosting_exit(1);
	consumer_task = xTaskCreateStatic(consumer, "consumer", 1024, NULL, 4, consumer_stack,
					  &consumer_tcb);
	controller_task = xTaskCreateStatic(controller, "controller", 1024, NULL, 2,
					    controller_stack, &controller_tcb);
	if (!consumer_task || !controller_task ||
	    ove_eventgroup_set_notify(group, notify, consumer_task) != OVE_OK)
		semihosting_exit(1);
	vTaskStartScheduler();
	semihosting_exit(2);
}
