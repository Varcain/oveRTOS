/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "task.h"
#include "ove/hal/hal_i2c.h"
#include "ove/sync.h"
#include "framework/semihosting_exit.h"
#include <stdio.h>
#include <string.h>

extern unsigned i2c_hal_calls;
extern uint16_t i2c_hal_address, i2c_hal_sizes[2];
extern uint32_t i2c_hal_frames[2];
static unsigned locks, callbacks, failures;
static uint8_t tx[65536], rx[65536];
static ove_i2c_storage_t storage;
int i2c_test_lock(ove_mutex_t mutex, uint64_t timeout)
{
	++locks;
	return ove_mutex_lock(mutex, timeout);
}
void vApplicationTickHook(void)
{
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	semihosting_exit(2);
}
static void check(int condition, const char *name)
{
	if (!condition) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
static void completed(int result, void *context)
{
	(void)context;
	check(result == OVE_OK, "async fixture result");
	++callbacks;
}
static void reset(void)
{
	i2c_hal_calls = locks = callbacks = 0;
}
static void run(void *argument)
{
	(void)argument;
	/* Only physical HAL I/O is substituted. Production frontend/provider,
     * vendor types and actual kernel locking/count conversion run here. */
	storage.hal_handle.State = HAL_I2C_STATE_READY;
	assert(ove_mutex_init(&storage.bus_mtx, &storage.bus_mtx_storage) == OVE_OK);
	reset();
	int result = ove_i2c_write(&storage, 0x50, tx, sizeof(tx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !locks && !i2c_hal_calls,
	      "write65536 rejected before mutex and uint16 HAL cast");
	reset();
	result = ove_i2c_read(&storage, 0x50, rx, sizeof(rx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !locks && !i2c_hal_calls,
	      "read65536 rejected before mutex and uint16 HAL cast");
	reset();
	result = ove_i2c_write_read(&storage, 0x50, tx, 3, rx, sizeof(rx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !locks && !i2c_hal_calls,
	      "combined RX65536 rejected before sequential HAL narrowing");
	reset();
	result = ove_i2c_write_read_async(&storage, 0x50, tx, sizeof(tx), rx, 1, completed, NULL);
	if (result == OVE_OK)
		ove_i2c_async_complete(&storage, OVE_OK);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls && !callbacks,
	      "async TX65536 rejected before publication and HAL cast");
	reset();
	result = ove_i2c_write_read_async(&storage, 0x50, tx, 1, rx, sizeof(rx), completed, NULL);
	if (result == OVE_OK)
		ove_i2c_async_complete(&storage, OVE_OK);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls && !callbacks,
	      "async RX65536 rejected before publication and retained count narrowing");
	reset();
	result = ove_i2c_write(&storage, 0x80, tx, 1, OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !locks && !i2c_hal_calls,
	      "address128 rejected before mutex or shifted HAL address");
	reset();
	result = ove_i2c_write(&storage, 0x7f, tx, UINT16_MAX, OVE_WAIT_FOREVER);
	check(result == OVE_OK && locks == 1 && i2c_hal_calls == 1 &&
		      i2c_hal_sizes[0] == UINT16_MAX && i2c_hal_address == 0xfe,
	      "65535 and address127 preserved exactly");
	reset();
	result = ove_i2c_write_read(&storage, 0x50, tx, 3, rx, UINT16_MAX, OVE_WAIT_FOREVER);
	check(result == OVE_OK && locks == 1 && i2c_hal_calls == 2 && i2c_hal_sizes[0] == 3 &&
		      i2c_hal_sizes[1] == UINT16_MAX && i2c_hal_frames[0] == I2C_FIRST_FRAME &&
		      i2c_hal_frames[1] == I2C_LAST_FRAME,
	      "independent phases retain FIRST/LAST repeated-start options");
	reset();
	result = ove_i2c_probe(&storage, 0x50, OVE_WAIT_FOREVER);
	check(result == OVE_OK && locks == 1 && i2c_hal_calls == 1 && !i2c_hal_sizes[0],
	      "zero-byte probe keeps established HAL path");
	reset();
	result = ove_hal_i2c_write(&storage, 0x50, tx, sizeof(tx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls,
	      "direct HAL write rejects wide count before narrowing");
	result = ove_hal_i2c_read(&storage, 0x50, rx, sizeof(rx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls,
	      "direct HAL read rejects wide count before narrowing");
	result = ove_hal_i2c_write_read(&storage, 0x50, tx, 1, rx, sizeof(rx), OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls,
	      "direct HAL combined call rejects wide count before narrowing");
	result = ove_hal_i2c_write(&storage, 128, tx, 1, OVE_WAIT_FOREVER);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls,
	      "direct HAL write rejects invalid address");
	ove_i2c_storage_t snapshot;
	memcpy(&snapshot, &storage, sizeof(storage));
	result = ove_hal_i2c_write_read_async(&storage, 0x50, tx, 1, rx, sizeof(rx));
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls &&
		      memcmp(&storage, &snapshot, sizeof(storage)) == 0,
	      "direct HAL async rejects wide RX without retained metadata changes");
	result = ove_hal_i2c_write_read_async(&storage, 128, tx, 1, rx, 1);
	check(result == OVE_ERR_INVALID_PARAM && !i2c_hal_calls &&
		      memcmp(&storage, &snapshot, sizeof(storage)) == 0,
	      "direct HAL async rejects invalid address before phase publication");
	ove_mutex_deinit(storage.bus_mtx);
	printf("ARM I2C native counts: failures=%u\n", failures);
	semihosting_exit(failures != 0);
}
int main(void)
{
	assert(xTaskCreate(run, "i2c-counts", 4096, NULL, 3, NULL) == pdPASS);
	vTaskStartScheduler();
	return 1;
}
