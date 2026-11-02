/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "ove/hal/hal_spi.h"
#include "ove/irq.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

SPI_TypeDef ove_test_spi_peripherals[5];
static _Atomic(SPI_HandleTypeDef *) pending[5];
#ifdef OVE_ARM
#include "framework/semihosting_exit.h"
static int in_tick;
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	semihosting_exit(1);
}
#else
static _Thread_local int in_tick;
#endif
static atomic_uint notifications, closes, queue_failures, isr_pends, heartbeats, hal_depth;
static TickType_t notification_tick[5];
static atomic_uint retry_visit;
static unsigned notification_visit[5];
static int failures;
#ifdef OVE_RUST_SPI_TEST
static atomic_uint rust_done;
static size_t rust_initial_heap;
extern void rust_spi_run(void);
void rust_spi_ready(void)
{
	rust_initial_heap = xPortGetFreeHeapSize();
}
void rust_spi_finished(void)
{
	atomic_store(&rust_done, 1);
}
void rust_spi_panic(void)
{
	semihosting_exit(2);
}
static void rust_executor(void *argument)
{
	(void)argument;
	rust_spi_run();
}
#endif
static _Alignas(2) uint8_t tx[4] = {1, 2, 3, 4}, rx[5][4];

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
int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	(void)value;
	return OVE_OK;
}
uint32_t HAL_GetTick(void)
{
	return (uint32_t)xTaskGetTickCount();
}
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle)
{
	(void)handle;
	return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle)
{
	(void)handle;
	atomic_fetch_add(&closes, 1);
	return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *handle, uint8_t *write, uint8_t *read,
					  uint16_t words, uint32_t timeout)
{
	(void)timeout;
	size_t bytes = (size_t)words * (handle->Init.DataSize == SPI_DATASIZE_16BIT ? 2 : 1);
	if (read) {
		if (write)
			memmove(read, write, bytes);
		else
			memset(read, 0, bytes);
	}
	return HAL_OK;
}
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{
	return HAL_SPI_TransmitReceive(h, p, NULL, n, t);
}
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n, uint32_t t)
{
	return HAL_SPI_TransmitReceive(h, NULL, p, n, t);
}
HAL_StatusTypeDef HAL_SPI_TransmitReceive_IT(SPI_HandleTypeDef *h, uint8_t *write, uint8_t *read,
					     uint16_t words)
{
	atomic_fetch_add(&hal_depth, 1);
	HAL_StatusTypeDef rc = HAL_SPI_TransmitReceive(h, write, read, words, 0);
	unsigned index = (unsigned)(h->Instance - ove_test_spi_peripherals);
	assert(index < 5);
	atomic_store(&pending[index], h);
	atomic_fetch_sub(&hal_depth, 1);
	return rc;
}
HAL_StatusTypeDef HAL_SPI_Transmit_IT(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n)
{
	return HAL_SPI_TransmitReceive_IT(h, p, NULL, n);
}
HAL_StatusTypeDef HAL_SPI_Receive_IT(SPI_HandleTypeDef *h, uint8_t *p, uint16_t n)
{
	return HAL_SPI_TransmitReceive_IT(h, NULL, p, n);
}
void vApplicationTickHook(void)
{
	in_tick = 1;
	for (unsigned i = 0; i < 5; ++i) {
		SPI_HandleTypeDef *h = atomic_exchange(&pending[i], NULL);
		if (h)
			HAL_SPI_TxRxCpltCallback(h);
	}
	in_tick = 0;
}
BaseType_t __real_xTimerPendFunctionCallFromISR(PendedFunction_t, void *, uint32_t, BaseType_t *);
BaseType_t __wrap_xTimerPendFunctionCallFromISR(PendedFunction_t callback, void *context,
						uint32_t value, BaseType_t *wake)
{
	atomic_fetch_add(&isr_pends, 1);
	assert(in_tick);
	BaseType_t rc = __real_xTimerPendFunctionCallFromISR(callback, context, value, wake);
	if (rc != pdPASS)
		atomic_fetch_add(&queue_failures, 1);
	return rc;
}
BaseType_t __real_xTimerPendFunctionCall(PendedFunction_t, void *, uint32_t, TickType_t);
BaseType_t __wrap_xTimerPendFunctionCall(PendedFunction_t callback, void *context, uint32_t value,
					 TickType_t wait)
{
	return __real_xTimerPendFunctionCall(callback, context, value, wait);
}
static void check(int value, const char *message)
{
	if (!value) {
		printf("FAIL: %s\n", message);
		++failures;
	}
}
static void completed(int result, void *context)
{
	unsigned index = (unsigned)(uintptr_t)context;
	unsigned count = atomic_fetch_add(&notifications, 1);
	if (count < 5) {
		notification_tick[count] = xTaskGetTickCount();
		notification_visit[count] = atomic_load(&retry_visit);
	}
	check(result == OVE_OK, "result reaches final callback");
	check(!in_tick && xTaskGetCurrentTaskHandle() == xTimerGetTimerDaemonTaskHandle(),
	      "actual timer-daemon task context");
	check(atomic_load(&hal_depth) == 0, "native submit/provider frame has retired");
	check(memcmp(tx, rx[index], sizeof(tx)) == 0, "actual bounded transfer data");
}
static void no_operation(void *context, uint32_t value)
{
	(void)context;
	(void)value;
}
static void heartbeat(TimerHandle_t timer)
{
	(void)timer;
	atomic_fetch_add(&heartbeats, 1);
}
static TimerCallbackFunction_t retry_function;
TimerHandle_t __real_xTimerCreateStatic(const char *, TickType_t, BaseType_t, void *,
					TimerCallbackFunction_t, StaticTimer_t *);
static void observe_retry(TimerHandle_t timer)
{
	atomic_fetch_add(&retry_visit, 1);
	retry_function(timer);
}
TimerHandle_t __wrap_xTimerCreateStatic(const char *name, TickType_t period, BaseType_t reload,
					void *id, TimerCallbackFunction_t callback,
					StaticTimer_t *storage)
{
	if (strcmp(name, "spi-retire") == 0) {
		retry_function = callback;
		callback = observe_retry;
	}
	return __real_xTimerCreateStatic(name, period, reload, id, callback, storage);
}
static void run_test(void *argument)
{
	(void)argument;
	check(ove_spi_async_prepare() == OVE_OK, "prestarted retirement service");
	static StaticTimer_t heartbeat_storage;
	TimerHandle_t other =
		xTimerCreateStatic("heartbeat", 3, pdTRUE, NULL, heartbeat, &heartbeat_storage);
	check(other && xTimerStart(other, 0) == pdPASS, "unrelated timer starts");
	vTaskDelay(2);
	size_t initial_heap = xPortGetFreeHeapSize();
	ove_spi_t handles[5] = {0};
	taskENTER_CRITICAL();
	for (unsigned i = 0; i < 5; ++i) {
		struct ove_spi_cfg cfg = {
			.instance = i, .word_size = i & 1 ? 16 : 8, .clock_hz = 1000000};
		int rc = ove_spi_create(&handles[i], &cfg);
		assert(rc == OVE_OK);
		rc = ove_spi_transfer_async(handles[i], NULL, tx, rx[i], sizeof(tx), completed,
					    (void *)(uintptr_t)i);
		assert(rc == OVE_OK);
	}
	unsigned queued = 0;
	while (xTimerPendFunctionCall(no_operation, NULL, 0, 0) == pdPASS)
		++queued;
	taskEXIT_CRITICAL();
	TickType_t deadline = xTaskGetTickCount() + 50;
	while (atomic_load(&isr_pends) < 5 && xTaskGetTickCount() < deadline) {
	}
	check(queued == configTIMER_QUEUE_LENGTH, "real timer queue filled");
	check(atomic_load(&isr_pends) == 5, "real tick ISR completed all native transfers");
	check(atomic_load(&queue_failures) >= 1, "real FromISR queue rejection observed");
	check(atomic_load(&notifications) == 0, "full daemon queue retains notifications");
	for (unsigned i = 0; i < 5; ++i)
		ove_spi_destroy(handles[i]);
	check(atomic_load(&closes) == 0, "pending destroy retains native resources");
	vTaskDelay(35);
	check(atomic_load(&notifications) == 5, "periodic kernel timer recovers all notifications");
	check(atomic_load(&closes) == 5, "all pending native handles closed once");
	check(xPortGetFreeHeapSize() == initial_heap, "actual native heap fully reclaimed");
	check(atomic_load(&heartbeats) > 1, "retry batches allow unrelated timer progress");
	for (unsigned i = 0; i < 4; ++i)
		check(notification_visit[i] == notification_visit[0],
		      "first retry drains one four-node batch");
	check(notification_visit[0] > 0 && notification_visit[4] > notification_visit[0],
	      "fifth node waits for a later real retry visit");
	printf("FreeRTOS SPI retirement: failures=%d callbacks=%u queue_failures=%u retry_visits=%u heap_before=%lu heap_after=%lu\n",
	       failures, atomic_load(&notifications), atomic_load(&queue_failures),
	       atomic_load(&retry_visit), (unsigned long)initial_heap,
	       (unsigned long)xPortGetFreeHeapSize());
#ifdef OVE_ARM
#ifdef OVE_RUST_SPI_TEST
	check(xTaskCreate(rust_executor, "rust-spi", 2048, NULL, 4, NULL) == pdPASS,
	      "Rust executor task starts");
	TickType_t rust_deadline = xTaskGetTickCount() + 200;
	while (!atomic_load(&rust_done) && xTaskGetTickCount() < rust_deadline)
		vTaskDelay(1);
	check(atomic_load(&rust_done), "normal Embassy task finishes owned transfers");
	vTaskDelay(10);
	check(xPortGetFreeHeapSize() == rust_initial_heap,
	      "Rust owner/staging and canceled-operation native heap reclaimed");
	printf("ARM Rust SPI: done=%u retained_runtime_heap=%lu current_heap=%lu\n",
	       atomic_load(&rust_done), (unsigned long)rust_initial_heap,
	       (unsigned long)xPortGetFreeHeapSize());
#endif
	unsigned long remaining = uxTaskGetStackHighWaterMark(xTimerGetTimerDaemonTaskHandle());
	printf("ARM timer daemon: reserved=%lu bytes remaining=%lu bytes\n",
	       (unsigned long)configTIMER_TASK_STACK_DEPTH * sizeof(StackType_t),
	       remaining * sizeof(StackType_t));
	semihosting_exit(failures != 0);
#else
	vTaskEndScheduler();
#endif
	for (;;) {
	}
}
int main(void)
{
	assert(xTaskCreate(run_test, "spi-test", 4096, NULL, 3, NULL) == pdPASS);
	vTaskStartScheduler();
	return failures ? 1 : 0;
}
