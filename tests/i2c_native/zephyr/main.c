/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/hal/hal_i2c.h"
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_t io_calls;
static unsigned failures;
static uint32_t sizes[2];
static uint8_t flags[2], message_count;
static uint16_t last_address;
static ove_i2c_storage_t storage;
static int configure(const struct device *device, uint32_t configuration)
{
	(void)device;
	(void)configuration;
	return 0;
}
static int transfer(const struct device *device, struct i2c_msg *messages, uint8_t count,
		    uint16_t address)
{
	(void)device;
	atomic_inc(&io_calls);
	message_count = count;
	last_address = address;
	for (unsigned i = 0; i < count && i < 2; ++i) {
		sizes[i] = messages[i].len;
		flags[i] = messages[i].flags;
	}
	return 0;
}
static const struct i2c_driver_api test_api = {.configure = configure, .transfer = transfer};
DEVICE_DT_DEFINE(DT_NODELABEL(i2c1), NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_I2C_INIT_PRIORITY,
		 &test_api);
static void check(int condition, const char *name)
{
	if (!condition) {
		++failures;
		printf("FAIL: %s\n", name);
	}
}
int main(void)
{
	ove_i2c_t i2c = NULL;
	const struct ove_i2c_cfg configuration = {.instance = 0, .speed = OVE_I2C_SPEED_FAST};
	check(ove_i2c_init(&i2c, &storage, &configuration) == OVE_OK, "actual kernel/device opens");
	if (!i2c)
		exit(1);
	check(sizeof(size_t) > sizeof(uint32_t), "host64 can express the over-limit count");
	size_t over = (size_t)UINT32_MAX + 1;
	check(ove_hal_i2c_validate_transfer(i2c, UINT32_MAX, UINT32_MAX, 0) == OVE_OK,
	      "uint32 boundary accepted independently");
	check(ove_hal_i2c_validate_transfer(i2c, over, 1, 0) == OVE_ERR_INVALID_PARAM,
	      "TX above uint32 rejected by actual provider");
	check(ove_hal_i2c_validate_transfer(i2c, 1, over, 1) == OVE_ERR_INVALID_PARAM,
	      "RX above uint32 rejected by actual provider");
	uint8_t tx[] = {1, 2, 3}, rx[7];
	check(ove_hal_i2c_write(i2c, 128, tx, sizeof(tx), 0) == OVE_ERR_INVALID_PARAM,
	      "direct write rejects invalid address");
	check(ove_hal_i2c_read(i2c, 128, rx, sizeof(rx), 0) == OVE_ERR_INVALID_PARAM,
	      "direct read rejects invalid address");
	check(ove_hal_i2c_write_read(i2c, 128, tx, sizeof(tx), rx, sizeof(rx), 0) ==
		      OVE_ERR_INVALID_PARAM,
	      "direct combined call rejects invalid address");
	ove_i2c_storage_t snapshot;
	memcpy(&snapshot, &storage, sizeof(snapshot));
	int result = ove_hal_i2c_write_read_async(i2c, 128, tx, sizeof(tx), rx, sizeof(rx));
	if (result == OVE_OK) {
		/* A rejected-path mutant may submit real work. Drain it before any
         * snapshot inspection so the test never races the kernel/handler. */
		struct k_work_sync sync;
		(void)k_work_flush(&storage.async_work, &sync);
	}
	check(result == OVE_ERR_INVALID_PARAM && memcmp(&snapshot, &storage, sizeof(storage)) == 0,
	      "direct async rejection preserves work and retained metadata");
	check(atomic_get(&io_calls) == 0, "invalid direct calls have no device I/O");
	check(ove_i2c_write_read(i2c, 0x7f, tx, sizeof(tx), rx, sizeof(rx), 0) == OVE_OK,
	      "valid repeated-start operation reaches actual driver API");
	check(atomic_get(&io_calls) == 1 && message_count == 2 && last_address == 0x7f &&
		      sizes[0] == sizeof(tx) && sizes[1] == sizeof(rx) &&
		      flags[0] == I2C_MSG_WRITE &&
		      flags[1] == (I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP),
	      "independent counts and repeated-start flags preserved");
	ove_i2c_deinit(i2c);
	printf("Zephyr I2C native boundary: failures=%u\n", failures);
	exit(failures != 0);
}
