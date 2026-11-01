/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/hal/hal_spi.h"
#include "ove/sync.h"
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { ALLOC, FREE, INIT, DEINIT, OPEN, CLOSE, HAL, LOCK, UNLOCK, GPIO, INVALID, STAT_COUNT };
struct segment {
	const void *tx;
	void *rx;
	size_t len;
	uint64_t timeout;
	unsigned int bits;
};
static unsigned int stats[STAT_COUNT];
static struct segment segments[1024];
static char trace[4096];
static size_t trace_len;
static unsigned int failure, fail_segment;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate, entered, released;
static _Thread_local ove_mutex_t held;
static ove_spi_storage_t raw_storage;
static ove_spi_t raw_handle;

static void event(char value)
{
	if (trace_len + 1 < sizeof(trace)) {
		trace[trace_len++] = value;
		trace[trace_len] = '\0';
	}
}

void spi_test_reset(void)
{
	memset(stats, 0, sizeof(stats));
	memset(segments, 0, sizeof(segments));
	trace_len = 0;
	trace[0] = '\0';
	failure = fail_segment = 0;
}

unsigned int spi_test_stat(unsigned int index)
{
	return index < STAT_COUNT ? stats[index] : 0;
}
const char *spi_test_trace(void)
{
	return trace;
}
void spi_test_fail(unsigned int stage)
{
	failure = stage;
}
void spi_test_fail_segment(unsigned int index)
{
	fail_segment = index;
}
size_t spi_test_segment(unsigned int index, unsigned int field)
{
	if (index >= 1024)
		return 0;
	const struct segment *s = &segments[index];
	switch (field) {
	case 0:
		return (size_t)s->tx;
	case 1:
		return (size_t)s->rx;
	case 2:
		return s->len;
	case 3:
		return (size_t)s->timeout;
	default:
		return s->bits;
	}
}

void *spi_test_malloc(size_t size)
{
	++stats[ALLOC];
	return failure == 1 ? NULL : malloc(size);
}
void spi_test_free(void *pointer)
{
	++stats[FREE];
	free(pointer);
}
int spi_test_mutex_init(ove_mutex_t *mutex, ove_mutex_storage_t *storage)
{
	pthread_mutex_lock(&state_lock);
	++stats[INIT];
	int cleaning = gate && entered && !released;
	pthread_mutex_unlock(&state_lock);
	/* Observe premature reuse without reinitializing a live pthread mutex. */
	if (cleaning)
		return OVE_ERR_BUSY;
	return failure == 2 ? OVE_ERR_NO_MEMORY : ove_mutex_init(mutex, storage);
}
void spi_test_mutex_deinit(ove_mutex_t mutex)
{
	++stats[DEINIT];
	ove_mutex_deinit(mutex);
}
int spi_test_lock(ove_mutex_t mutex, uint64_t timeout)
{
	int rc = ove_mutex_lock(mutex, timeout);
	if (rc == OVE_OK) {
		if (held != NULL)
			++stats[INVALID];
		held = mutex;
		++stats[LOCK];
		event('L');
	}
	return rc;
}
void spi_test_unlock(ove_mutex_t mutex)
{
	if (held != mutex)
		++stats[INVALID];
	held = NULL;
	++stats[UNLOCK];
	event('U');
	ove_mutex_unlock(mutex);
}
int spi_test_gpio(unsigned int port, unsigned int pin, int value)
{
	if (port != 2 || pin != 7 || held == NULL)
		++stats[INVALID];
	++stats[GPIO];
	event(value ? 'D' : 'A');
	return OVE_OK;
}
int ove_hal_spi_open(ove_spi_t spi, const struct ove_spi_cfg *cfg)
{
	pthread_mutex_lock(&state_lock);
	++stats[OPEN];
	pthread_mutex_unlock(&state_lock);
	if (cfg->word_size != 8 && cfg->word_size != 16)
		++stats[INVALID];
	spi->fd = 1;
	return failure == 3 ? OVE_ERR_IO : OVE_OK;
}
void ove_hal_spi_close(ove_spi_t spi)
{
	pthread_mutex_lock(&state_lock);
	++stats[CLOSE];
	if (gate) {
		entered = 1;
		pthread_cond_broadcast(&gate_cv);
		while (!released)
			pthread_cond_wait(&gate_cv, &state_lock);
	}
	pthread_mutex_unlock(&state_lock);
	spi->fd = -1;
}
int ove_hal_spi_validate_transfer(ove_spi_t spi, size_t len, int asynchronous)
{
	(void)spi;
	(void)len;
	(void)asynchronous;
	return OVE_OK;
}
int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout)
{
	unsigned int index = stats[HAL]++;
	event('H');
	if (held != spi->bus_mtx)
		++stats[INVALID];
	/* The real POSIX mutex must be held, independently of the forwarding trace. */
	int rc = pthread_mutex_trylock(&spi->bus_mtx->mtx);
	if (rc != EBUSY) {
		++stats[INVALID];
		if (rc == 0)
			pthread_mutex_unlock(&spi->bus_mtx->mtx);
	}
	if (index < 1024)
		segments[index] = (struct segment){tx, rx, len, timeout, spi->word_size};
	if (fail_segment != 0 && index + 1 == fail_segment)
		return OVE_ERR_IO;
	if (rx != NULL) {
		if (tx != NULL)
			memmove(rx, tx, len);
		else
			memset(rx, 0xa5, len);
	}
	return OVE_OK;
}
int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	(void)spi;
	(void)tx;
	(void)rx;
	(void)len;
	return OVE_ERR_NOT_SUPPORTED;
}
void spi_test_gate_start(void)
{
	gate = 1;
	entered = released = 0;
}
void spi_test_gate_wait(void)
{
	pthread_mutex_lock(&state_lock);
	while (!entered)
		pthread_cond_wait(&gate_cv, &state_lock);
	pthread_mutex_unlock(&state_lock);
}
void spi_test_gate_release(void)
{
	pthread_mutex_lock(&state_lock);
	released = 1;
	gate = 0;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&state_lock);
}
void *spi_test_raw(unsigned int bits)
{
	struct ove_spi_cfg cfg = {.clock_hz = 1000000, .word_size = (uint8_t)bits};
	return ove_spi_init(&raw_handle, &raw_storage, &cfg) == OVE_OK ? raw_handle : NULL;
}
void spi_test_raw_close(void)
{
	ove_spi_deinit(raw_handle);
}
int spi_test_malformed(void *handle, unsigned int misalign)
{
	uint16_t words[4] = {0};
	return ove_spi_transfer(handle, NULL, (char *)words + misalign, NULL, misalign ? 2 : 1, 0);
}
size_t spi_test_layout(unsigned int field)
{
	switch (field) {
	case 0:
		return sizeof(struct ove_spi_xfer);
	case 1:
		return _Alignof(struct ove_spi_xfer);
	case 2:
		return sizeof(ove_spi_storage_t);
	case 3:
		return _Alignof(ove_spi_storage_t);
	case 4:
		return offsetof(struct ove_spi_xfer, tx);
	case 5:
		return offsetof(struct ove_spi_xfer, rx);
	default:
		return offsetof(struct ove_spi_xfer, len);
	}
}
