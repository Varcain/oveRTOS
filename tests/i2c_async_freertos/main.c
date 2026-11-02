/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "ove/hal/hal_i2c.h"
#include "ove/irq.h"
#include "framework/semihosting_exit.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static I2C_TypeDef peripherals[4];
static _Atomic(I2C_HandleTypeDef *) active[4];
static atomic_uint callbacks, closes, ticks, failed_kicks, isr_kicks, task_kicks, service_visit,
	model_count, heartbeats;
static unsigned model_visits[5];
static int model_mode, fail_create = 1, fail_start = 1;
static unsigned service_creates;
static int in_tick, hal_depth, failures, expected_result, fail_receive, fail_transmit, inject_nack;
static uint8_t tx[3] = {1, 2, 3}, rx[7];
void I2C1_EV_IRQHandler(void);
void I2C2_EV_IRQHandler(void);
void I2C3_EV_IRQHandler(void);
void I2C4_EV_IRQHandler(void);
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	semihosting_exit(2);
}
ove_irq_key_t ove_irq_lock(void)
{
	if (in_tick)
		return (UINT64_C(1) << 63) | taskENTER_CRITICAL_FROM_ISR();
	taskENTER_CRITICAL();
	return 0;
}
void ove_irq_unlock(ove_irq_key_t key)
{
	if (key & (UINT64_C(1) << 63))
		taskEXIT_CRITICAL_FROM_ISR((UBaseType_t)key);
	else
		taskEXIT_CRITICAL();
}
bool ove_is_in_isr(void)
{
	return in_tick != 0;
}
uint32_t HAL_GetTick(void)
{
	return atomic_load(&ticks);
}
HAL_StatusTypeDef __wrap_HAL_I2C_Init(I2C_HandleTypeDef *h)
{
	I2C_TypeDef *instances[] = {I2C1, I2C2, I2C3, I2C4};
	unsigned index = 0;
	while (index < 4 && h->Instance != instances[index])
		++index;
	assert(index < 4);
	memset(&peripherals[index], 0, sizeof(peripherals[index]));
	h->Instance = &peripherals[index];
	h->State = HAL_I2C_STATE_READY;
	h->Lock = HAL_UNLOCKED;
	h->Mode = HAL_I2C_MODE_NONE;
	return HAL_OK;
}
HAL_StatusTypeDef __wrap_HAL_I2C_DeInit(I2C_HandleTypeDef *h)
{
	(void)h;
	atomic_fetch_add(&closes, 1);
	return HAL_OK;
}
static void check(int value, const char *message)
{
	if (!value) {
		printf("FAIL: %s\n", message);
		++failures;
	}
}
static void require(int value, const char *message)
{
	check(value, message);
	if (!value)
		semihosting_exit(1);
}
static void completed(int result, void *context)
{
	(void)context;
	check(result == expected_result, "real vendor HAL result reaches callback");
	check(!in_tick && xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle(),
	      "actual timer-daemon callback context");
	check(hal_depth == 0, "callback follows real vendor HAL IRQ final access");
	atomic_fetch_add(&callbacks, 1);
}
void vApplicationTickHook(void)
{
	in_tick = 1;
	atomic_fetch_add(&ticks, 1);
	for (unsigned index = 0; index < 4; ++index) {
		I2C_TypeDef *peripheral = &peripherals[index];
		I2C_HandleTypeDef *h = atomic_load(&active[index]);
		if (h) {
			if (inject_nack) {
				peripheral->ISR = I2C_ISR_NACKF | I2C_ISR_STOPF;
				inject_nack = 0;
			} else if (h->XferCount) {
				if (h->State == HAL_I2C_STATE_BUSY_TX)
					peripheral->ISR = I2C_ISR_TXIS;
				else {
					peripheral->RXDR = 0x5a;
					peripheral->ISR = I2C_ISR_RXNE;
				}
			} else
				peripheral->ISR = h->XferOptions == I2C_FIRST_FRAME ? I2C_ISR_TC
										    : I2C_ISR_STOPF;
			++hal_depth;
			void (*handlers[])(void) = {I2C1_EV_IRQHandler, I2C2_EV_IRQHandler,
						    I2C3_EV_IRQHandler, I2C4_EV_IRQHandler};
			handlers[index]();
			--hal_depth;
			peripheral->ISR = 0;
			if (h->State == HAL_I2C_STATE_READY && h->XferISR == NULL)
				atomic_store(&active[index], NULL);
		}
	}
	in_tick = 0;
}
static PendedFunction_t original_pended;
static TimerCallbackFunction_t original_retry;
static void observe_pended(void *argument, uint32_t value)
{
	atomic_fetch_add(&service_visit, 1);
	original_pended(argument, value);
}
static void observe_retry(TimerHandle_t timer)
{
	atomic_fetch_add(&service_visit, 1);
	original_retry(timer);
}
BaseType_t __real_xTimerPendFunctionCallFromISR(PendedFunction_t, void *, uint32_t, BaseType_t *);
BaseType_t __wrap_xTimerPendFunctionCallFromISR(PendedFunction_t fn, void *arg, uint32_t value,
						BaseType_t *wake)
{
	original_pended = fn;
	atomic_fetch_add(&isr_kicks, 1);
	check(arg == NULL && value == 0, "permanent dispatch arguments only");
	BaseType_t rc = __real_xTimerPendFunctionCallFromISR(observe_pended, arg, value, wake);
	if (rc != pdPASS)
		atomic_fetch_add(&failed_kicks, 1);
	return rc;
}
BaseType_t __real_xTimerPendFunctionCall(PendedFunction_t, void *, uint32_t, TickType_t);
BaseType_t __wrap_xTimerPendFunctionCall(PendedFunction_t fn, void *arg, uint32_t value,
					 TickType_t wait)
{
	original_pended = fn;
	atomic_fetch_add(&task_kicks, 1);
	check(arg == NULL && value == 0, "permanent task dispatch arguments only");
	BaseType_t rc = __real_xTimerPendFunctionCall(observe_pended, arg, value, wait);
	if (rc != pdPASS)
		atomic_fetch_add(&failed_kicks, 1);
	return rc;
}
TimerHandle_t __real_xTimerCreateStatic(const char *, TickType_t, BaseType_t, void *,
					TimerCallbackFunction_t, StaticTimer_t *);
TimerHandle_t __wrap_xTimerCreateStatic(const char *name, TickType_t period, BaseType_t reload,
					void *id, TimerCallbackFunction_t fn,
					StaticTimer_t *storage)
{
	if (strcmp(name, "i2c-retire") == 0) {
		++service_creates;
		check(ove_i2c_async_prepare() == OVE_ERR_BUSY, "preparation reentry is busy");
		if (fail_create) {
			fail_create = 0;
			return NULL;
		}
		original_retry = fn;
		fn = observe_retry;
	}
	return __real_xTimerCreateStatic(name, period, reload, id, fn, storage);
}
BaseType_t __real_xTimerGenericCommandFromTask(TimerHandle_t, BaseType_t, TickType_t, BaseType_t *,
					       TickType_t);
BaseType_t __wrap_xTimerGenericCommandFromTask(TimerHandle_t timer, BaseType_t command,
					       TickType_t value, BaseType_t *wake, TickType_t wait)
{
	if (command == tmrCOMMAND_START && strcmp(pcTimerGetName(timer), "i2c-retire") == 0 &&
	    fail_start) {
		fail_start = 0;
		check(ove_i2c_async_prepare() == OVE_ERR_BUSY, "start preparation reentry is busy");
		return pdFAIL;
	}
	return __real_xTimerGenericCommandFromTask(timer, command, value, wake, wait);
}
void __real_ove_i2c_async_retired(ove_i2c_t bus);
void __wrap_ove_i2c_async_retired(ove_i2c_t bus)
{
	if (model_mode) {
		unsigned count = atomic_load_explicit(&model_count, memory_order_relaxed);
		if (count < 5)
			model_visits[count] = atomic_load(&service_visit);
		atomic_store_explicit(&model_count, count + 1, memory_order_release);
		return;
	}
	__real_ove_i2c_async_retired(bus);
}
static void no_operation(void *arg, uint32_t value)
{
	(void)arg;
	(void)value;
}
static void heartbeat(TimerHandle_t timer)
{
	(void)timer;
	atomic_fetch_add(&heartbeats, 1);
}
HAL_StatusTypeDef __real_HAL_I2C_Master_Seq_Receive_IT(I2C_HandleTypeDef *, uint16_t, uint8_t *,
						       uint16_t, uint32_t);
HAL_StatusTypeDef __wrap_HAL_I2C_Master_Seq_Receive_IT(I2C_HandleTypeDef *h, uint16_t addr,
						       uint8_t *data, uint16_t size,
						       uint32_t options)
{
	if (fail_receive) {
		fail_receive = 0;
		return HAL_BUSY;
	}
	return __real_HAL_I2C_Master_Seq_Receive_IT(h, addr, data, size, options);
}
HAL_StatusTypeDef __real_HAL_I2C_Master_Seq_Transmit_IT(I2C_HandleTypeDef *, uint16_t, uint8_t *,
							uint16_t, uint32_t);
HAL_StatusTypeDef __wrap_HAL_I2C_Master_Seq_Transmit_IT(I2C_HandleTypeDef *h, uint16_t addr,
							uint8_t *data, uint16_t size,
							uint32_t options)
{
	if (fail_transmit) {
		fail_transmit = 0;
		return HAL_BUSY;
	}
	return __real_HAL_I2C_Master_Seq_Transmit_IT(h, addr, data, size, options);
}
static void phase_failure_cases(void)
{
	for (unsigned kind = 0; kind < 3; ++kind) {
		static ove_i2c_storage_t storage;
		ove_i2c_t bus;
		struct ove_i2c_cfg cfg = {.speed = OVE_I2C_SPEED_FAST};
		require(ove_i2c_init(&bus, &storage, &cfg) == OVE_OK,
			"error case controller initializes");
		unsigned before = atomic_load(&callbacks);
		expected_result = kind == 2 ? OVE_ERR_BUS_NACK : OVE_ERR_BUS_BUSY;
		fail_transmit = kind == 0;
		fail_receive = kind == 1;
		inject_nack = kind == 2;
		int rc = ove_i2c_write_read_async(bus, 0x42, tx, sizeof(tx), rx, sizeof(rx),
						  completed, NULL);
		if (kind == 0) {
			require(rc == OVE_ERR_BUS_BUSY,
				"initial HAL start failure returned without callback");
			check(atomic_load(&callbacks) == before && storage.pending_cb == NULL &&
				      !__atomic_load_n(&storage.async_busy, __ATOMIC_ACQUIRE),
			      "rejected HAL start publishes no callback");
		} else {
			require(rc == OVE_OK, "error operation accepted");
			atomic_store(&active[0], &storage.hal_handle);
			TickType_t deadline = xTaskGetTickCount() + 100;
			while ((atomic_load(&callbacks) == before || atomic_load(&active[0])) &&
			       xTaskGetTickCount() < deadline)
				vTaskDelay(1);
			require(atomic_load(&callbacks) == before + 1 && !atomic_load(&active[0]),
				"error callback and HAL IRQ retire before reuse");
			check(storage.hal_handle.State == HAL_I2C_STATE_READY,
			      "error resets native HAL state");
		}
		check(storage.async_rx == NULL && storage.async_rx_len == 0 &&
			      storage.async_addr == 0,
		      "phase metadata clears on start rejection and final error");
		ove_i2c_deinit(bus);
	}
	expected_result = OVE_OK;
}
static void queue_case(void)
{
	size_t baseline = xPortGetFreeHeapSize();
	unsigned previous_callbacks = atomic_load(&callbacks),
		 previous_closes = atomic_load(&closes);
	unsigned previous_kicks = atomic_load(&isr_kicks),
		 previous_failed = atomic_load(&failed_kicks);
	ove_i2c_t owners[4];
	taskENTER_CRITICAL();
	for (unsigned i = 0; i < 4; ++i) {
		struct ove_i2c_cfg cfg = {.instance = i, .speed = OVE_I2C_SPEED_FAST};
		assert(ove_i2c_create(&owners[i], &cfg) == OVE_OK);
		assert(ove_i2c_write_read_async(owners[i], 0x42, tx, sizeof(tx), NULL, 0, completed,
						NULL) == OVE_OK);
		atomic_store(&active[i], &owners[i]->hal_handle);
		ove_i2c_destroy(owners[i]);
	}
	unsigned queued = 0;
	while (__real_xTimerPendFunctionCall(no_operation, NULL, 0, 0) == pdPASS)
		++queued;
	taskEXIT_CRITICAL();
	TickType_t deadline = xTaskGetTickCount() + 50;
	while (atomic_load(&isr_kicks) < previous_kicks + 4 && xTaskGetTickCount() < deadline) {
	}
	check(queued == configTIMER_QUEUE_LENGTH, "actual daemon queue filled");
	check(atomic_load(&failed_kicks) == previous_failed + 4,
	      "all full-queue ISR kicks rejected");
	check(atomic_load(&callbacks) == previous_callbacks &&
		      atomic_load(&closes) == previous_closes,
	      "four pending owners survive full queue");
	deadline = xTaskGetTickCount() + 100;
	while (atomic_load(&callbacks) < previous_callbacks + 4 && xTaskGetTickCount() < deadline)
		vTaskDelay(1);
	require(atomic_load(&callbacks) == previous_callbacks + 4,
		"permanent retry recovers four actual HAL completions");
	check(atomic_load(&closes) == previous_closes + 4, "four pending destroys finish once");
	check(xPortGetFreeHeapSize() == baseline,
	      "actual controller and mutex heap fully recovers");
}
static void list_service_case(void)
{
	/* Service-only nodes: no peripheral opens or fifth physical controller.
     * Replace only the final consumer to measure native list drain batches. */
	static ove_i2c_storage_t nodes[5];
	model_mode = 1;
	unsigned kicks = atomic_load(&task_kicks), beat = atomic_load(&heartbeats);
	for (unsigned i = 0; i < 5; ++i)
		ove_hal_i2c_async_retire(&nodes[i]);
	check(atomic_load(&task_kicks) == kicks + 1, "five list nodes coalesce one permanent kick");
	TickType_t deadline = xTaskGetTickCount() + 100;
	while (atomic_load(&model_count) < 5 && xTaskGetTickCount() < deadline)
		vTaskDelay(1);
	require(atomic_load(&model_count) == 5, "all service-only list nodes retire");
	for (unsigned i = 1; i < 4; ++i)
		check(model_visits[i] == model_visits[0], "first four nodes share one drain");
	check(model_visits[4] > model_visits[0],
	      "fifth service node waits for another bounded drain");
	check(atomic_load(&task_kicks) == kicks + 1, "drain does not refill its daemon queue");
	check(atomic_load(&heartbeats) > beat, "unrelated timer progresses");
	model_mode = 0;
}
static void run_test(void *argument)
{
	(void)argument;
	require(ove_i2c_async_prepare() == OVE_ERR_NO_MEMORY,
		"failed static timer creation reported");
	require(ove_i2c_async_prepare() == OVE_ERR_NO_MEMORY, "failed retry timer start reported");
	require(ove_i2c_async_prepare() == OVE_OK, "failed preparation can recover");
	require(service_creates == 2, "start retry reuses one static timer");
	check(!atomic_load(&callbacks) && !atomic_load(&closes),
	      "preparation publishes no operation");
	for (unsigned kind = 0; kind < 5; ++kind) {
		static ove_i2c_storage_t storage;
		ove_i2c_t bus = NULL;
		struct ove_i2c_cfg cfg = {.speed = OVE_I2C_SPEED_FAST};
		require(ove_i2c_init(&bus, &storage, &cfg) == OVE_OK,
			"real controller initialized");
		unsigned before = atomic_load(&callbacks);
		memset(rx, 0, sizeof(rx));
		int rc = ove_i2c_write_read_async(bus, 0x42, kind == 1 ? NULL : tx,
						  kind == 1   ? 0
						  : kind == 3 ? 1
						  : kind == 4 ? 2
							      : sizeof(tx),
						  kind == 0 ? NULL : rx, kind == 0 ? 0 : sizeof(rx),
						  completed, NULL);
		require(rc == OVE_OK, "real HAL accepts independent TX/RX phases");
		atomic_store(&active[0], &storage.hal_handle);
		TickType_t deadline = xTaskGetTickCount() + 100;
		while ((atomic_load(&callbacks) == before || atomic_load(&active[0])) &&
		       xTaskGetTickCount() < deadline)
			vTaskDelay(1);
		check(atomic_load(&active[0]) == NULL, "real HAL IRQ sequence finishes");
		check(atomic_load(&callbacks) == before + 1,
		      "one final callback per real HAL sequence");
		if (atomic_load(&active[0]) != NULL || atomic_load(&callbacks) != before + 1)
			semihosting_exit(1);
		if (kind)
			for (size_t i = 0; i < sizeof(rx); ++i)
				check(rx[i] == 0x5a, "independent complete RX span");
		ove_i2c_deinit(bus);
	}
	check(atomic_load(&closes) == 5, "each completed controller closes once");
	phase_failure_cases();
	static StaticTimer_t heartbeat_storage;
	TimerHandle_t timer =
		xTimerCreateStatic("heartbeat", 3, pdTRUE, NULL, heartbeat, &heartbeat_storage);
	check(timer && xTimerStart(timer, 0) == pdPASS, "unrelated timer starts");
	vTaskDelay(5);

#ifndef OVE_I2C_CAPTURE_BARRIER
	queue_case();
	vTaskDelay(5);
	list_service_case();
#endif
	printf("Real STM32 HAL I2C retirement: %d failed assertion(s), daemon stack %u bytes free\n",
	       failures,
	       (unsigned)uxTaskGetStackHighWaterMark(xTimerGetTimerDaemonTaskHandle()) *
		       (unsigned)sizeof(StackType_t));
	semihosting_exit(failures ? 1 : 0);
}
int main(void)
{
	if (xTaskCreate(run_test, "test", 2048, NULL, 3, NULL) != pdPASS)
		semihosting_exit(2);
	vTaskStartScheduler();
	return 1;
}
