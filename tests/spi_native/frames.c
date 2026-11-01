/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include "ove/hal/hal_spi.h"
#include "ove/gpio.h"
#include "ove/sync.h"
#ifdef OVE_TEST_SPI_STM32
#include "stm32f7xx_hal.h"
#else
#include <pthread.h>
#include "nuttx/spi/spi_transfer.h"
#endif

static _Alignas(2) uint8_t tx_buffer[131080];
static _Alignas(2) uint8_t rx_buffer[131080];
static ove_spi_storage_t storage;
static ove_spi_t spi;
static const struct ove_spi_cs cs = {.gpio_port = 2, .gpio_pin = 3, .active_low = 1};

static struct {
	const uint8_t *tx;
	uint8_t *rx;
	size_t tx_len, rx_len, bytes;
	unsigned int calls, invalid, locked, selected, workers;
	unsigned int adapter_calls;
	unsigned int error_call, completions;
	int result, asynchronous;
	uint32_t tick, spent, timeouts[8];
	size_t native_words[8];
	const void *native_tx[8];
	void *native_rx[8];
	char trace[64];
	size_t trace_len;
} probe;

static void event(char value)
{
	if (probe.trace_len + 1 < sizeof(probe.trace)) {
		probe.trace[probe.trace_len++] = value;
		probe.trace[probe.trace_len] = 0;
	}
}

int ove_mutex_init(ove_mutex_t *mutex, ove_mutex_storage_t *backing)
{
	*mutex = backing;
	return OVE_OK;
}

void ove_mutex_deinit(ove_mutex_t mutex)
{
	(void)mutex;
}

int ove_mutex_lock(ove_mutex_t mutex, uint64_t timeout)
{
	(void)mutex;
	(void)timeout;
	if (probe.locked++)
		++probe.invalid;
	event('L');
	return OVE_OK;
}

void ove_mutex_unlock(ove_mutex_t mutex)
{
	(void)mutex;
	if (probe.locked != 1)
		++probe.invalid;
	probe.locked = 0;
	event('U');
}

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	if (port != cs.gpio_port || pin != cs.gpio_pin)
		++probe.invalid;
	if (!value) {
		if (probe.selected++)
			++probe.invalid;
		event('A');
	} else {
		if (probe.selected != 1)
			++probe.invalid;
		probe.selected = 0;
		event('D');
	}
	return OVE_OK;
}

static int fits(const void *pointer, size_t length, const void *base, size_t capacity)
{
	uintptr_t address = (uintptr_t)pointer, start = (uintptr_t)base;
	return address >= start && address - start <= capacity &&
	       length <= capacity - (address - start);
}

int ove_test_spi_native_transfer(ove_spi_t handle, const void *tx, void *rx, size_t len,
				 uint64_t timeout);

int ove_hal_spi_transfer(ove_spi_t handle, const void *tx, void *rx, size_t len, uint64_t timeout)
{
	++probe.adapter_calls;
	if (!len) {
		++probe.invalid;
		return OVE_ERR_INVALID_PARAM;
	}
	return ove_test_spi_native_transfer(handle, tx, rx, len, timeout);
}

/* The native doubles consume native words, independently of the adapter's
 * byte interpretation. Check logical spans before touching backing memory. */
static int native_transfer(const void *tx, void *rx, size_t words, unsigned int bits,
			   uint32_t timeout)
{
	event('H');
	if (probe.calls < 8) {
		probe.timeouts[probe.calls] = timeout;
		probe.native_words[probe.calls] = words;
		probe.native_tx[probe.calls] = tx;
		probe.native_rx[probe.calls] = rx;
	}
	++probe.calls;
	size_t bytes = words * (bits / 8);
	if (!words || !probe.selected || probe.locked != (unsigned int)!probe.asynchronous ||
	    (!tx && !rx) || (tx && !fits(tx, bytes, probe.tx, probe.tx_len)) ||
	    (rx && !fits(rx, bytes, probe.rx, probe.rx_len)) ||
	    (tx && (uintptr_t)tx != (uintptr_t)probe.tx + probe.bytes) ||
	    (rx && (uintptr_t)rx != (uintptr_t)probe.rx + probe.bytes)) {
		++probe.invalid;
		return -1;
	}
	if (probe.error_call == probe.calls)
		return -1;
	if (rx) {
		if (tx)
			memmove(rx, tx, bytes);
		else
			memset(rx, 0xa5, bytes);
	}
	probe.bytes += bytes;
	probe.tick += probe.spent;
	return 0;
}

#ifdef OVE_TEST_SPI_STM32
SPI_TypeDef ove_test_spi_peripherals[5];

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *handle)
{
	(void)handle;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_DeInit(SPI_HandleTypeDef *handle)
{
	(void)handle;
	return HAL_OK;
}

uint32_t HAL_GetTick(void)
{
	return probe.tick;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *handle, uint8_t *tx, uint8_t *rx,
					  uint16_t words, uint32_t timeout)
{
	return native_transfer(tx, rx, words, handle->Init.DataSize, timeout) ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *handle, uint8_t *tx, uint16_t words,
				   uint32_t timeout)
{
	return HAL_SPI_TransmitReceive(handle, tx, NULL, words, timeout);
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *handle, uint8_t *rx, uint16_t words,
				  uint32_t timeout)
{
	return HAL_SPI_TransmitReceive(handle, NULL, rx, words, timeout);
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive_IT(SPI_HandleTypeDef *handle, uint8_t *tx, uint8_t *rx,
					     uint16_t words)
{
	HAL_StatusTypeDef result = HAL_SPI_TransmitReceive(handle, tx, rx, words, 0);
	if (result == HAL_OK)
		HAL_SPI_TxRxCpltCallback(handle);
	return result;
}

HAL_StatusTypeDef HAL_SPI_Transmit_IT(SPI_HandleTypeDef *handle, uint8_t *tx, uint16_t words)
{
	return HAL_SPI_TransmitReceive_IT(handle, tx, NULL, words);
}

HAL_StatusTypeDef HAL_SPI_Receive_IT(SPI_HandleTypeDef *handle, uint8_t *rx, uint16_t words)
{
	return HAL_SPI_TransmitReceive_IT(handle, NULL, rx, words);
}
#else
int ove_test_spi_ioctl(int fd, unsigned long request, ...);
int ove_test_spi_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
				void *(*entry)(void *), void *argument);

int ove_test_spi_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	va_list arguments;
	va_start(arguments, request);
	struct spi_sequence_s *sequence = (struct spi_sequence_s *)va_arg(arguments, unsigned long);
	va_end(arguments);
	if (request != SPIIOC_TRANSFER || sequence->ntrans != 1) {
		++probe.invalid;
		return -1;
	}
	return native_transfer(sequence->trans->txbuffer, sequence->trans->rxbuffer,
			       sequence->trans->nwords, sequence->nbits, 0);
}

int ove_test_spi_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
				void *(*entry)(void *), void *argument)
{
	(void)attr;
	++probe.workers;
	*thread = pthread_self();
	(void)entry(argument);
	return 0;
}
#endif

static void prepare(unsigned int bits, const void *tx, size_t tx_len, void *rx, size_t rx_len)
{
	memset(&storage, 0, sizeof(storage));
	struct ove_spi_cfg config = {.word_size = bits, .clock_hz = 1000000};
#ifdef OVE_TEST_SPI_STM32
	assert_int_equal(ove_spi_init(&spi, &storage, &config), OVE_OK);
#else
	/* A native descriptor is dependency-provided; no device is opened. */
	storage.word_size = config.word_size;
	storage.clock_hz = config.clock_hz;
	storage.bus_mtx = &storage.bus_mtx_storage;
	storage.fd = -1;
	spi = &storage;
#endif
	memset(&probe, 0, sizeof(probe));
	probe.tx = tx;
	probe.tx_len = tx_len;
	probe.rx = rx;
	probe.rx_len = rx_len;
}

static void assert_rejected(int result)
{
	assert_int_equal(result, OVE_ERR_INVALID_PARAM);
	assert_int_equal(probe.calls + probe.workers + probe.trace_len, 0);
	assert_int_equal(probe.invalid + probe.locked + probe.selected, 0);
}

static void test_native_counts_are_bytes(void **state)
{
	(void)state;
	for (unsigned int bits = 8; bits <= 16; bits += 8) {
		for (unsigned int direction = 0; direction < 3; ++direction) {
			const void *tx = direction == 2 ? NULL : tx_buffer;
			void *rx = direction == 1 ? NULL : rx_buffer;
			prepare(bits, tx, 4, rx, 4);
			int result = ove_spi_transfer(spi, &cs, tx, rx, 4, OVE_MS(20));
			assert_int_equal(result, OVE_OK);
			assert_int_equal(probe.bytes, 4);
			assert_int_equal(probe.calls, 1);
			assert_int_equal(probe.invalid, 0);
			assert_string_equal(probe.trace, "LAHDU");
		}
	}
}

static void test_invalid_frames_have_no_effects(void **state)
{
	(void)state;
	for (unsigned int invalid = 0; invalid < 5; ++invalid) {
		void *tx = invalid == 1 ? tx_buffer + 1 : tx_buffer;
		void *rx = invalid == 2 ? rx_buffer + 1 : rx_buffer;
		size_t length = invalid == 0 ? 3 : 4;
		if (invalid == 3)
			tx = rx = NULL;
		if (invalid == 4)
			rx = (uint8_t *)tx + 2;
		prepare(16, tx, length, rx, length);
		assert_rejected(ove_spi_transfer(spi, &cs, tx, rx, length, OVE_MS(20)));
	}
	for (unsigned int bits = 8; bits <= 16; bits += 8) {
		for (unsigned int reverse = 0; reverse < 2; ++reverse) {
			void *tx = tx_buffer + (reverse ? bits / 8 : 0);
			void *rx = tx_buffer + (reverse ? 0 : bits / 8);
			prepare(bits, tx, 4, rx, 4);
			assert_rejected(ove_spi_transfer(spi, &cs, tx, rx, 4, OVE_MS(20)));
		}
	}
}

static void test_entire_sequence_is_preflighted(void **state)
{
	(void)state;
	for (unsigned int invalid = 0; invalid < 3; ++invalid) {
		prepare(16, tx_buffer, 8, rx_buffer, 8);
		struct ove_spi_xfer sequence[] = {{tx_buffer, rx_buffer, 2},
						  {NULL, NULL, 0},
						  {tx_buffer + 2, rx_buffer + 2, 2}};
		if (invalid == 0)
			sequence[2].len = 1;
		else if (invalid == 1)
			sequence[2].tx = tx_buffer + 3;
		else
			sequence[2].tx = sequence[2].rx = NULL;
		assert_rejected(ove_spi_transfer_seq(spi, &cs, sequence, 3, OVE_MS(20)));
	}
}

static void test_large_native_count_is_preserved(void **state)
{
	(void)state;
	for (unsigned int bits = 8; bits <= 16; bits += 8) {
		size_t bytes = 65536u * (bits / 8);
		for (unsigned int direction = 0; direction < 3; ++direction) {
			const uint8_t *tx = direction == 2 ? NULL : tx_buffer;
			uint8_t *rx = direction == 1 ? NULL : rx_buffer;
			prepare(bits, tx, bytes, rx, bytes);
			for (size_t i = 0; i < bytes; ++i)
				tx_buffer[i] = (uint8_t)(i * 13 + 7);
			memset(rx_buffer, 0xcc, bytes);
			probe.tick = UINT32_MAX - 3;
			probe.spent = 5;
			int result = ove_spi_transfer(spi, &cs, tx, rx, bytes, OVE_MS(20));
			assert_int_equal(result, OVE_OK);
			assert_int_equal(probe.bytes, bytes);
			assert_int_equal(probe.invalid + probe.locked + probe.selected, 0);
#ifdef OVE_TEST_SPI_STM32
			assert_int_equal(probe.calls, 2);
			assert_int_equal(probe.native_words[0], 65535);
			assert_int_equal(probe.native_words[1], 1);
			assert_ptr_equal(probe.native_tx[0], tx);
			assert_ptr_equal(probe.native_rx[0], rx);
			assert_ptr_equal(probe.native_tx[1], tx ? tx + 65535u * (bits / 8) : NULL);
			assert_ptr_equal(probe.native_rx[1], rx ? rx + 65535u * (bits / 8) : NULL);
			assert_int_equal(probe.timeouts[0], 20);
			assert_int_equal(probe.timeouts[1], 15);
			assert_string_equal(probe.trace, "LAHHDU");
#else
			assert_int_equal(probe.calls, 1);
			assert_int_equal(probe.native_words[0], 65536);
			assert_string_equal(probe.trace, "LAHDU");
#endif
			if (tx && rx)
				assert_memory_equal(rx, tx, bytes);
			else if (rx) {
				assert_int_equal(rx[0], 0xa5);
				assert_int_equal(rx[bytes - 1], 0xa5);
			}
		}
	}
}

static void check_stm32_timeout_policies(void)
{
#ifdef OVE_TEST_SPI_STM32
	for (unsigned int policy = 0; policy < 3; ++policy) {
		prepare(16, tx_buffer, 131072, rx_buffer, 131072);
		probe.spent = 25;
		uint64_t timeout = policy == 0 ? OVE_WAIT_FOREVER : policy == 1 ? 0 : OVE_MS(20);
		int result = ove_spi_transfer(spi, &cs, tx_buffer, rx_buffer, 131072, timeout);
		assert_int_equal(result, policy == 0 ? OVE_OK : OVE_ERR_TIMEOUT);
		assert_int_equal(probe.calls, policy == 0 ? 2 : 1);
		assert_int_equal(probe.bytes, policy == 0 ? 131072 : 131070);
		assert_int_equal(probe.timeouts[0], policy == 0	  ? HAL_MAX_DELAY
						    : policy == 1 ? 0
								  : 20);
		assert_int_equal(probe.invalid + probe.locked + probe.selected, 0);
		assert_string_equal(probe.trace, policy == 0 ? "LAHHDU" : "LAHDU");
	}
#endif
}

static void test_in_place_and_error_cleanup(void **state)
{
	(void)state;
	prepare(16, rx_buffer, 4, rx_buffer, 4);
	memcpy(rx_buffer, "abcd", 4);
	assert_int_equal(ove_spi_transfer(spi, &cs, rx_buffer, rx_buffer, 4, OVE_MS(20)), OVE_OK);
	assert_memory_equal(rx_buffer, "abcd", 4);
	assert_int_equal(probe.bytes, 4);
	assert_int_equal(probe.invalid, 0);
	prepare(16, tx_buffer, 8, rx_buffer, 8);
	struct ove_spi_xfer skipped[] = {{NULL, NULL, 0},
					 {tx_buffer, rx_buffer, 2},
					 {NULL, NULL, 0},
					 {tx_buffer + 2, rx_buffer + 2, 2}};
	assert_int_equal(ove_spi_transfer_seq(spi, &cs, skipped, 4, OVE_MS(20)), OVE_OK);
	assert_int_equal(probe.bytes, 4);
	assert_int_equal(probe.calls, 2);
	assert_int_equal(probe.adapter_calls, 2);
	assert_int_equal(probe.invalid, 0);
	assert_string_equal(probe.trace, "LAHHDU");
	prepare(16, tx_buffer, 8, rx_buffer, 8);
	probe.error_call = 2;
	struct ove_spi_xfer sequence[] = {{tx_buffer, rx_buffer, 2},
					  {tx_buffer + 2, rx_buffer + 2, 2}};
	assert_int_equal(ove_spi_transfer_seq(spi, &cs, sequence, 2, OVE_MS(20)),
			 OVE_ERR_BUS_ERROR);
	assert_int_equal(probe.bytes, 2);
	assert_int_equal(probe.calls, 2);
	assert_int_equal(probe.invalid + probe.locked + probe.selected, 0);
	assert_string_equal(probe.trace, "LAHHDU");
	check_stm32_timeout_policies();
}

static void complete(int result, void *data)
{
	if (data != &probe || probe.selected || probe.locked)
		++probe.invalid;
	++probe.completions;
	probe.result = result;
}

static void test_async_frames_and_admission(void **state)
{
	(void)state;
	for (unsigned int bits = 8; bits <= 16; bits += 8) {
		for (unsigned int direction = 0; direction < 3; ++direction) {
			const void *tx = direction == 2 ? NULL : tx_buffer;
			void *rx = direction == 1 ? NULL : rx_buffer;
			prepare(bits, tx, 4, rx, 4);
			probe.asynchronous = 1;
			assert_int_equal(ove_spi_transfer_async(spi, &cs, tx, rx, 4, complete,
								&probe),
					 OVE_OK);
			assert_int_equal(probe.completions, 1);
			assert_int_equal(probe.result, OVE_OK);
			assert_int_equal(probe.bytes, 4);
			assert_int_equal(probe.invalid, 0);
		}
	}
	for (unsigned int invalid = 0; invalid < 3; ++invalid) {
		prepare(16, tx_buffer, 4, rx_buffer, 4);
		void *tx = invalid == 1 ? tx_buffer + 1 : tx_buffer;
		size_t length = invalid == 0 ? 3 : 4;
#ifdef OVE_TEST_SPI_STM32
		if (invalid == 2)
			length = 131072;
#else
		if (invalid == 2)
			tx = NULL;
#endif
		void *rx = invalid == 2 && !tx ? NULL : rx_buffer;
		storage.async_busy = 1;
		storage.pending_cb = complete;
		storage.pending_cs = &cs;
		storage.pending_ud = tx_buffer;
		ove_spi_storage_t before;
		memcpy(&before, &storage, sizeof(before));
		assert_rejected(ove_spi_transfer_async(spi, &cs, tx, rx, length, complete, &probe));
		assert_memory_equal(&before, &storage, sizeof(before));
	}
#ifdef OVE_TEST_SPI_STM32
	for (unsigned int bits = 8; bits <= 16; bits += 8) {
		size_t length = 65535u * (bits / 8);
		prepare(bits, tx_buffer, length, rx_buffer, length);
		probe.asynchronous = 1;
		assert_int_equal(ove_spi_transfer_async(spi, &cs, tx_buffer, rx_buffer, length,
							complete, &probe),
				 OVE_OK);
		assert_int_equal(probe.bytes, length);
		assert_int_equal(probe.calls, 1);
		assert_int_equal(probe.invalid, 0);
		prepare(bits, tx_buffer, length + bits / 8, rx_buffer, length + bits / 8);
		assert_rejected(ove_spi_transfer_async(spi, &cs, tx_buffer, rx_buffer,
						       length + bits / 8, complete, &probe));
	}
#endif
}

int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_native_counts_are_bytes),
		cmocka_unit_test(test_invalid_frames_have_no_effects),
		cmocka_unit_test(test_entire_sequence_is_preflighted),
		cmocka_unit_test(test_large_native_count_is_preserved),
		cmocka_unit_test(test_in_place_and_error_cleanup),
		cmocka_unit_test(test_async_frames_and_admission),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
