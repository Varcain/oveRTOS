/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <cmocka.h>
#include "ove/hal/hal_i2c.h"
#include "ove/sync.h"

static unsigned locks, io_calls, callbacks;
static size_t last_tx, last_rx;
static size_t provider_limit;
static uint16_t last_address;
static uint8_t tx[65536], rx[65536];

int i2c_test_lock(ove_mutex_t mutex, uint64_t timeout)
{
	++locks;
	return ove_mutex_lock(mutex, timeout);
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *config)
{
	(void)i2c;
	(void)config;
	return OVE_OK;
}
void ove_hal_i2c_close(ove_i2c_t i2c)
{
	(void)i2c;
}
int ove_hal_i2c_validate_transfer(ove_i2c_t i2c, size_t tx_len, size_t rx_len, int asynchronous)
{
	(void)i2c;
	(void)asynchronous;
	return tx_len <= provider_limit && rx_len <= provider_limit ? OVE_OK
								    : OVE_ERR_INVALID_PARAM;
}
static int record(uint16_t address, size_t tx_len, size_t rx_len)
{
	++io_calls;
	last_address = address;
	last_tx = tx_len;
	last_rx = rx_len;
	return OVE_OK;
}
int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t address, const void *data, size_t len,
		      uint64_t timeout)
{
	(void)i2c;
	(void)data;
	(void)timeout;
	return record(address, len, 0);
}
int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t address, void *data, size_t len, uint64_t timeout)
{
	(void)i2c;
	(void)data;
	(void)timeout;
	return record(address, 0, len);
}
int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t address, const void *write, size_t write_len,
			   void *read, size_t read_len, uint64_t timeout)
{
	(void)i2c;
	(void)write;
	(void)read;
	(void)timeout;
	return record(address, write_len, read_len);
}
int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t address, const void *write,
				 size_t write_len, void *read, size_t read_len)
{
	(void)i2c;
	(void)write;
	(void)read;
	return record(address, write_len, read_len);
}
static void completed(int result, void *context)
{
	(void)result;
	(void)context;
	++callbacks;
}

enum operation { WRITE, READ, WRITE_READ, REG_WRITE, REG_READ, PROBE, ASYNC };
struct boundary_case {
	const char *name;
	enum operation operation;
	uint16_t address;
	size_t tx_len, rx_len;
	int expected;
	size_t provider_limit;
};
static const struct boundary_case cases[] = {
	{"write_address_128", WRITE, 128, 1, 0, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"read_address_65535", READ, UINT16_MAX, 0, 1, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"combined_address_128", WRITE_READ, 128, 1, 1, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"register_write_address_128", REG_WRITE, 128, 1, 0, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"register_read_address_128", REG_READ, 128, 0, 1, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"probe_address_128", PROBE, 128, 0, 0, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"async_address_128", ASYNC, 128, 1, 1, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"write_length_65536", WRITE, 0x50, sizeof(tx), 0, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"read_length_65536", READ, 0x50, 0, sizeof(rx), OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"combined_tx_length_65536", WRITE_READ, 0x50, sizeof(tx), 1, OVE_ERR_INVALID_PARAM,
	 UINT16_MAX},
	{"combined_rx_length_65536", WRITE_READ, 0x50, 1, sizeof(rx), OVE_ERR_INVALID_PARAM,
	 UINT16_MAX},
	{"register_read_length_65536", REG_READ, 0x50, 0, sizeof(rx), OVE_ERR_INVALID_PARAM,
	 UINT16_MAX},
	{"async_tx_length_65536", ASYNC, 0x50, sizeof(tx), 1, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"async_rx_length_65536", ASYNC, 0x50, 1, sizeof(rx), OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"write_address_127", WRITE, 127, 1, 0, OVE_OK, UINT16_MAX},
	{"write_address_zero", WRITE, 0, 1, 0, OVE_OK, UINT16_MAX},
	{"write_length_65535", WRITE, 0x50, UINT16_MAX, 0, OVE_OK, UINT16_MAX},
	{"read_length_65535", READ, 0x50, 0, UINT16_MAX, OVE_OK, UINT16_MAX},
	{"independent_lengths_tx_long", WRITE_READ, 0x50, UINT16_MAX, 1, OVE_OK, UINT16_MAX},
	{"independent_lengths_rx_long", WRITE_READ, 0x50, 1, UINT16_MAX, OVE_OK, UINT16_MAX},
	{"empty_prefix_is_read", WRITE_READ, 0x50, 0, 1, OVE_OK, UINT16_MAX},
	{"probe_zero_bytes", PROBE, 0x50, 0, 0, OVE_OK, UINT16_MAX},
	{"write_zero_bytes", WRITE, 0x50, 0, 0, OVE_OK, UINT16_MAX},
	{"register_write_prefix_included", REG_WRITE, 0x50, OVE_I2C_REG_WRITE_MAX, 0, OVE_OK,
	 UINT16_MAX},
	{"register_write_over_capacity", REG_WRITE, 0x50, OVE_I2C_REG_WRITE_MAX + 1, 0,
	 OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"async_write_only", ASYNC, 0x50, UINT16_MAX, 0, OVE_OK, UINT16_MAX},
	{"async_read_only", ASYNC, 0x50, 0, UINT16_MAX, OVE_OK, UINT16_MAX},
	{"async_all_empty", ASYNC, 0x50, 0, 0, OVE_ERR_INVALID_PARAM, UINT16_MAX},
	{"register_prefix_counts_toward_provider_limit", REG_WRITE, 0x50, OVE_I2C_REG_WRITE_MAX, 0,
	 OVE_ERR_INVALID_PARAM, OVE_I2C_REG_WRITE_MAX},
};

static void boundary(void **state)
{
	const struct boundary_case *test = *state;
	provider_limit = test->provider_limit;
	ove_i2c_storage_t storage;
	memset(&storage, 0, sizeof(storage));
	ove_i2c_t i2c = NULL;
	const struct ove_i2c_cfg config = {.instance = 0, .speed = OVE_I2C_SPEED_STANDARD};
	assert_int_equal(ove_i2c_init(&i2c, &storage, &config), OVE_OK);
	locks = io_calls = callbacks = 0;
	last_tx = last_rx = 0;
	ove_i2c_storage_t snapshot;
	memcpy(&snapshot, &storage, sizeof(snapshot));
	int result = OVE_ERR_IO;
	switch (test->operation) {
	case WRITE:
		result = ove_i2c_write(i2c, test->address, test->tx_len ? tx : NULL, test->tx_len,
				       0);
		break;
	case READ:
		result = ove_i2c_read(i2c, test->address, rx, test->rx_len, 0);
		break;
	case WRITE_READ:
		result = ove_i2c_write_read(i2c, test->address, test->tx_len ? tx : NULL,
					    test->tx_len, rx, test->rx_len, 0);
		break;
	case REG_WRITE:
		result = ove_i2c_reg_write(i2c, test->address, 0x12, tx, test->tx_len, 0);
		break;
	case REG_READ:
		result = ove_i2c_reg_read(i2c, test->address, 0x12, rx, test->rx_len, 0);
		break;
	case PROBE:
		result = ove_i2c_probe(i2c, test->address, 0);
		break;
	case ASYNC:
		result = ove_i2c_write_read_async(i2c, test->address, test->tx_len ? tx : NULL,
						  test->tx_len, test->rx_len ? rx : NULL,
						  test->rx_len, completed, &storage);
		if (result == OVE_OK)
			ove_i2c_async_complete(i2c, OVE_OK);
		break;
	}
	unsigned observed_locks = locks, observed_io = io_calls, observed_callbacks = callbacks;
	int unchanged = memcmp(&snapshot, &storage, sizeof(storage)) == 0;
	ove_i2c_deinit(i2c);
	assert_int_equal(result, test->expected);
	if (result == OVE_OK) {
		assert_int_equal(observed_io, 1);
		assert_int_equal(observed_locks, test->operation == ASYNC ? 0 : 1);
		assert_int_equal(observed_callbacks, test->operation == ASYNC ? 1 : 0);
		assert_int_equal(last_address, test->address);
		size_t expected_tx = test->operation == REG_WRITE  ? test->tx_len + 1
				     : test->operation == REG_READ ? 1
								   : test->tx_len;
		assert_int_equal(last_tx, expected_tx);
		assert_int_equal(last_rx, test->rx_len);
	} else {
		if (test->operation == ASYNC)
			assert_true(unchanged);
		assert_int_equal(observed_locks, 0);
		assert_int_equal(observed_io, 0);
		assert_int_equal(observed_callbacks, 0);
	}
}
static void pending_metadata_survives_validation_failure(void **state)
{
	(void)state;
	provider_limit = UINT16_MAX;
	ove_i2c_storage_t storage = {0}, snapshot;
	ove_i2c_t i2c = NULL;
	const struct ove_i2c_cfg config = {.instance = 0, .speed = OVE_I2C_SPEED_STANDARD};
	assert_int_equal(ove_i2c_init(&i2c, &storage, &config), OVE_OK);
	locks = io_calls = callbacks = 0;
	assert_int_equal(ove_i2c_write_read_async(i2c, 0x50, tx, 1, rx, 1, completed, &storage),
			 OVE_OK);
	memcpy(&snapshot, &storage, sizeof(storage));
	int address_result =
		ove_i2c_write_read_async(i2c, 128, tx, 1, rx, 1, completed, &callbacks);
	int length_result =
		ove_i2c_write_read_async(i2c, 0x50, tx, sizeof(tx), rx, 1, completed, &callbacks);
	int unchanged = memcmp(&snapshot, &storage, sizeof(storage)) == 0;
	unsigned calls = io_calls, locked = locks;
	ove_i2c_async_complete(i2c, OVE_OK);
	ove_i2c_deinit(i2c);
	assert_int_equal(address_result, OVE_ERR_INVALID_PARAM);
	assert_int_equal(length_result, OVE_ERR_INVALID_PARAM);
	assert_true(unchanged);
	assert_int_equal(calls, 1);
	assert_int_equal(locked, 0);
	assert_int_equal(callbacks, 1);
}
int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	struct CMUnitTest tests[sizeof(cases) / sizeof(cases[0]) + 1];
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
		tests[i] = (struct CMUnitTest){.name = cases[i].name,
					       .test_func = boundary,
					       .initial_state = (void *)&cases[i]};
	tests[sizeof(cases) / sizeof(cases[0])] =
		(struct CMUnitTest){.name = "pending_metadata_survives_validation_failure",
				    .test_func = pending_metadata_survives_validation_failure};
	return _cmocka_run_group_tests("I2C native preflight", tests,
				       sizeof(tests) / sizeof(tests[0]), NULL, NULL);
}
