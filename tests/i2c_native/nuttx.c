/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <cmocka.h>
#include <nuttx/i2c/i2c_master.h>
#include "ove/hal/hal_i2c.h"

static struct i2c_msg_s observed[2];
static size_t count;
static unsigned closes, io_calls, worker_creates;
int i2c_test_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
			    void *(*entry)(void *), void *argument)
{
	(void)thread;
	(void)attributes;
	(void)entry;
	(void)argument;
	++worker_creates;
	return EAGAIN;
}
int i2c_test_open(const char *path, int flags, ...)
{
	(void)flags;
	assert_string_equal(path, "/dev/i2c0");
	return 17;
}
int i2c_test_close(int fd)
{
	assert_int_equal(fd, 17);
	++closes;
	return 0;
}
int i2c_test_ioctl(int fd, unsigned long command, ...)
{
	++io_calls;
	assert_int_equal(fd, 17);
	assert_int_equal(command, I2CIOC_TRANSFER);
	va_list arguments;
	va_start(arguments, command);
	struct i2c_transfer_s *transfer = (void *)va_arg(arguments, unsigned long);
	va_end(arguments);
	assert_true(transfer->msgc <= 2);
	count = transfer->msgc;
	memcpy(observed, transfer->msgv, count * sizeof(observed[0]));
	return 0;
}
static void repeated_start(void **state)
{
	(void)state;
	ove_i2c_storage_t storage;
	memset(&storage, 0, sizeof(storage));
	storage.speed_hz = 400000;
	const struct ove_i2c_cfg config = {.instance = 0, .speed = OVE_I2C_SPEED_FAST};
	assert_int_equal(ove_hal_i2c_open(&storage, &config), OVE_OK);
	uint8_t tx[] = {0xa8, 0x20, 0x12}, rx[7];
	int result = ove_hal_i2c_write_read(&storage, 0x38, tx, sizeof(tx), rx, sizeof(rx), 0);
	ove_hal_i2c_close(&storage);
	assert_int_equal(result, OVE_OK);
	assert_int_equal(count, 2);
	assert_int_equal(closes, 1);
	assert_int_equal(observed[0].frequency, 400000);
	assert_int_equal(observed[1].frequency, 400000);
	assert_int_equal(observed[0].addr, 0x38);
	assert_int_equal(observed[1].addr, 0x38);
	assert_ptr_equal(observed[0].buffer, tx);
	assert_ptr_equal(observed[1].buffer, rx);
	assert_int_equal(observed[0].length, sizeof(tx));
	assert_int_equal(observed[1].length, sizeof(rx));
	assert_int_equal(observed[0].flags, I2C_M_NOSTOP);
	assert_int_equal(observed[1].flags, I2C_M_READ);
}
static void provider_limits(void **state)
{
	(void)state;
	ove_i2c_storage_t storage = {0};
	size_t accepted = (size_t)INT_MAX < (size_t)SSIZE_MAX ? (size_t)INT_MAX : (size_t)SSIZE_MAX;
	assert_int_equal(ove_hal_i2c_validate_transfer(&storage, accepted, accepted, 0), OVE_OK);
	assert_int_equal(ove_hal_i2c_validate_transfer(&storage, accepted, accepted, 1), OVE_OK);
	assert_int_equal(ove_hal_i2c_validate_transfer(&storage, accepted + 1, 0, 0),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_hal_i2c_validate_transfer(&storage, 0, accepted + 1, 1),
			 OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_hal_i2c_validate_transfer(&storage, SIZE_MAX, 1, 1),
			 OVE_ERR_INVALID_PARAM);
}
static void direct_invalid_address(void **state)
{
	(void)state;
	ove_i2c_storage_t storage = {0};
	storage.fd = 17;
	storage.speed_hz = 400000;
	uint8_t tx[] = {1}, rx[1];
	unsigned before = io_calls, workers = worker_creates;
	assert_int_equal(ove_hal_i2c_write(&storage, 128, tx, 1, 0), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_hal_i2c_read(&storage, 128, rx, 1, 0), OVE_ERR_INVALID_PARAM);
	assert_int_equal(ove_hal_i2c_write_read(&storage, 128, tx, 1, rx, 1, 0),
			 OVE_ERR_INVALID_PARAM);
	ove_i2c_storage_t snapshot;
	memcpy(&snapshot, &storage, sizeof(storage));
	assert_int_equal(ove_hal_i2c_write_read_async(&storage, 128, tx, 1, rx, 1),
			 OVE_ERR_INVALID_PARAM);
	assert_memory_equal(&storage, &snapshot, sizeof(storage));
	assert_int_equal(io_calls, before);
	assert_int_equal(worker_creates, workers);
}
int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	const struct CMUnitTest tests[] = {cmocka_unit_test(repeated_start),
					   cmocka_unit_test(provider_limits),
					   cmocka_unit_test(direct_invalid_address)};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
