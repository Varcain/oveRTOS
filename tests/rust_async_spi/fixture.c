/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/hal/hal_spi.h"
#include "ove/irq.h"
#include "ove/sync.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct async_spi_stats {
	size_t allocations, frees, live, bytes, peak_bytes, attempts;
	size_t submissions, completions, opens, closes, asserted, deasserted;
	size_t mutex_inits, mutex_deinits;
};
struct async_spi_job {
	ove_spi_t spi;
	const void *tx;
	void *rx;
	size_t bytes;
	unsigned word_bits;
};
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static struct async_spi_stats stats;
static struct async_spi_job jobs[16];
static struct {
	void *pointer;
	size_t bytes;
} allocations[128];
static int allocation_failure = -1, prepare_error, open_error, mode;
static size_t word_limit = SIZE_MAX, last_bytes, last_words, transmitted_size;
static unsigned char transmitted[1024], transmitted_tail;
static unsigned last_port;
static _Thread_local unsigned irq_depth;
static atomic_int local_irq_model;

ove_irq_key_t __real_ove_irq_lock(void);
void __real_ove_irq_unlock(ove_irq_key_t key);
ove_irq_key_t __wrap_ove_irq_lock(void)
{
	ove_irq_key_t key = atomic_load(&local_irq_model) ? irq_depth : __real_ove_irq_lock();
	++irq_depth;
	return key;
}
void __wrap_ove_irq_unlock(ove_irq_key_t key)
{
	assert(irq_depth != 0);
	--irq_depth;
	if (!atomic_load(&local_irq_model))
		__real_ove_irq_unlock(key);
}
unsigned async_spi_probe_irq_depth(void)
{
	return irq_depth;
}
void async_spi_probe_local_irq_model(int enabled)
{
	atomic_store(&local_irq_model, enabled);
}

void async_spi_probe_reset(void)
{
	pthread_mutex_lock(&control);
	assert(stats.live == 0);
	for (unsigned i = 0; i < 16; ++i)
		assert(jobs[i].spi == NULL);
	memset(&stats, 0, sizeof(stats));
	memset(allocations, 0, sizeof(allocations));
	allocation_failure = -1;
	prepare_error = open_error = mode = 0;
	word_limit = SIZE_MAX;
	last_bytes = last_words = transmitted_size = 0;
	atomic_store(&local_irq_model, 0);
	pthread_mutex_unlock(&control);
}

void async_spi_probe_stats(struct async_spi_stats *out)
{
	pthread_mutex_lock(&control);
	*out = stats;
	pthread_mutex_unlock(&control);
}

void async_spi_probe_fail_alloc(int after)
{
	pthread_mutex_lock(&control);
	allocation_failure = after;
	pthread_mutex_unlock(&control);
}
void async_spi_probe_prepare_error(int error)
{
	pthread_mutex_lock(&control);
	prepare_error = error;
	pthread_mutex_unlock(&control);
}
void async_spi_probe_open_error(int error)
{
	pthread_mutex_lock(&control);
	open_error = error;
	pthread_mutex_unlock(&control);
}
void async_spi_probe_mode(int value)
{
	pthread_mutex_lock(&control);
	mode = value;
	pthread_mutex_unlock(&control);
}
void async_spi_probe_limit(size_t words)
{
	pthread_mutex_lock(&control);
	word_limit = words;
	pthread_mutex_unlock(&control);
}

int async_spi_test_mutex_init(ove_mutex_t *mutex, ove_mutex_storage_t *storage)
{
	int result = ove_mutex_init(mutex, storage);
	if (result == OVE_OK) {
		pthread_mutex_lock(&control);
		++stats.mutex_inits;
		pthread_mutex_unlock(&control);
	}
	return result;
}
void async_spi_test_mutex_deinit(ove_mutex_t mutex)
{
	ove_mutex_deinit(mutex);
	pthread_mutex_lock(&control);
	++stats.mutex_deinits;
	pthread_mutex_unlock(&control);
}

void *async_spi_test_malloc(size_t bytes)
{
	pthread_mutex_lock(&control);
	++stats.attempts;
	if (allocation_failure == 0) {
		pthread_mutex_unlock(&control);
		return NULL;
	}
	if (allocation_failure > 0)
		--allocation_failure;
	void *pointer = malloc(bytes);
	if (pointer) {
		unsigned slot;
		for (slot = 0; slot < 128 && allocations[slot].pointer; ++slot) {
		}
		assert(slot < 128);
		allocations[slot].pointer = pointer;
		allocations[slot].bytes = bytes;
		++stats.allocations;
		++stats.live;
		stats.bytes += bytes;
		if (stats.bytes > stats.peak_bytes)
			stats.peak_bytes = stats.bytes;
	}
	pthread_mutex_unlock(&control);
	return pointer;
}

void async_spi_test_free(void *pointer)
{
	if (!pointer)
		return;
	pthread_mutex_lock(&control);
	unsigned slot;
	for (slot = 0; slot < 128 && allocations[slot].pointer != pointer; ++slot) {
	}
	assert(slot < 128);
	stats.bytes -= allocations[slot].bytes;
	allocations[slot].pointer = NULL;
	allocations[slot].bytes = 0;
	--stats.live;
	++stats.frees;
	pthread_mutex_unlock(&control);
	/* Real free: successful fixed-only lifecycle controls are not quarantined. */
	free(pointer);
}

int ove_gpio_set(unsigned int port, unsigned int pin, int value)
{
	(void)port;
	(void)pin;
	pthread_mutex_lock(&control);
	last_port = port;
	if (value)
		++stats.deasserted;
	else
		++stats.asserted;
	pthread_mutex_unlock(&control);
	return OVE_OK;
}

int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *config)
{
	(void)config;
	pthread_mutex_lock(&control);
	int error = open_error;
	if (!error)
		++stats.opens;
	pthread_mutex_unlock(&control);
	if (!error)
		spi->fd = 1;
	return error;
}

void ove_hal_spi_close(ove_spi_t spi)
{
	pthread_mutex_lock(&control);
	++stats.closes;
	pthread_mutex_unlock(&control);
	spi->fd = -1;
}

int ove_hal_spi_async_prepare(void)
{
	pthread_mutex_lock(&control);
	int error = prepare_error;
	pthread_mutex_unlock(&control);
	return error;
}
int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t bytes, int asynchronous)
{
	(void)asynchronous;
	pthread_mutex_lock(&control);
	int result = bytes / (spi->word_size / 8) > word_limit ? OVE_ERR_INVALID_PARAM : OVE_OK;
	pthread_mutex_unlock(&control);
	return result;
}
int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t bytes, uint64_t timeout)
{
	(void)spi;
	(void)tx;
	(void)rx;
	(void)bytes;
	(void)timeout;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_spi_async_retire(ove_spi_t spi)
{
	ove_spi_async_retired(spi);
}

void async_spi_probe_complete(int result)
{
	pthread_mutex_lock(&control);
	unsigned slot;
	for (slot = 0; slot < 16 && !jobs[slot].spi; ++slot) {
	}
	assert(slot < 16);
	struct async_spi_job job = jobs[slot];
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	transmitted_size = job.bytes < sizeof(transmitted) ? job.bytes : sizeof(transmitted);
	if (job.tx) {
		memcpy(transmitted, job.tx, transmitted_size);
		transmitted_tail = ((const unsigned char *)job.tx)[job.bytes - 1];
	} else {
		memset(transmitted, 0xa5, transmitted_size);
		transmitted_tail = 0xa5;
	}
	if (job.rx) {
		if (job.tx)
			memmove(job.rx, job.tx, job.bytes);
		else
			memset(job.rx, 0xa5, job.bytes);
	}
	if (result != OVE_OK && job.rx)
		memset(job.rx, 0xee, job.bytes);
	++stats.completions;
	pthread_mutex_unlock(&control);
	/* Unlink/cache before the real common completion, permitting reentry/free. */
	ove_spi_async_complete(job.spi, result);
}

int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t bytes)
{
	pthread_mutex_lock(&control);
	++stats.submissions;
	int behavior = mode;
	if (behavior == 2) {
		pthread_mutex_unlock(&control);
		return OVE_ERR_IO;
	}
	unsigned slot;
	for (slot = 0; slot < 16 && jobs[slot].spi; ++slot) {
	}
	assert(slot < 16);
	jobs[slot] = (struct async_spi_job){spi, tx, rx, bytes, spi->word_size};
	last_bytes = bytes;
	last_words = bytes / (spi->word_size / 8);
	pthread_mutex_unlock(&control);
	if (behavior == 1)
		async_spi_probe_complete(OVE_OK);
	return OVE_OK;
}

size_t async_spi_probe_last_bytes(void)
{
	pthread_mutex_lock(&control);
	size_t value = last_bytes;
	pthread_mutex_unlock(&control);
	return value;
}
size_t async_spi_probe_last_words(void)
{
	pthread_mutex_lock(&control);
	size_t value = last_words;
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_spi_probe_tx_byte(size_t index)
{
	pthread_mutex_lock(&control);
	assert(index < transmitted_size);
	unsigned value = transmitted[index];
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_spi_probe_tx_tail(void)
{
	pthread_mutex_lock(&control);
	unsigned value = transmitted_tail;
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_spi_probe_last_port(void)
{
	pthread_mutex_lock(&control);
	unsigned value = last_port;
	pthread_mutex_unlock(&control);
	return value;
}
uintptr_t async_spi_probe_pending_tx(void)
{
	pthread_mutex_lock(&control);
	uintptr_t value = 0;
	for (unsigned i = 0; i < 16; ++i)
		if (jobs[i].spi) {
			value = (uintptr_t)jobs[i].tx;
			break;
		}
	pthread_mutex_unlock(&control);
	return value;
}
uintptr_t async_spi_probe_pending_rx(void)
{
	pthread_mutex_lock(&control);
	uintptr_t value = 0;
	for (unsigned i = 0; i < 16; ++i)
		if (jobs[i].spi) {
			value = (uintptr_t)jobs[i].rx;
			break;
		}
	pthread_mutex_unlock(&control);
	return value;
}
