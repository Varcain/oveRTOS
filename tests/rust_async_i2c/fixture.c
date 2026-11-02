/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/hal/hal_i2c.h"
#include "ove/irq.h"
#include "ove/sync.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct async_i2c_stats {
	size_t allocations, frees, live, bytes, peak_bytes, attempts;
	size_t submissions, completions, opens, closes, invalid, prepares;
	size_t mutex_inits, mutex_deinits;
};
struct async_i2c_job {
	uintptr_t i2c, tx, rx;
	size_t tx_len, rx_len;
	unsigned address;
};
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static struct async_i2c_stats stats;
static struct async_i2c_job jobs[16];
static struct {
	void *pointer;
	size_t bytes;
} allocations[128];
static int allocation_failure = -1, prepare_error, open_error, mutex_error, mode;
static size_t phase_limit = SIZE_MAX, last_tx_len, last_rx_len, transmitted_size;
static unsigned char transmitted[1024], transmitted_tail;
static unsigned last_instance, last_speed;
static _Thread_local unsigned irq_depth;
static atomic_int local_irq_model;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int hold_deinit, deinit_entered, deinit_released;
static uintptr_t configured_handle;

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
unsigned async_i2c_probe_irq_depth(void)
{
	return irq_depth;
}
void async_i2c_probe_local_irq_model(int enabled)
{
	atomic_store(&local_irq_model, enabled);
}

void async_i2c_probe_clear_watch(void);
void async_i2c_probe_reset(void)
{
	async_i2c_probe_clear_watch();
	pthread_mutex_lock(&control);
	assert(stats.live == 0);
	for (unsigned i = 0; i < 16; ++i)
		assert(jobs[i].i2c == 0);
	memset(&stats, 0, sizeof(stats));
	memset(allocations, 0, sizeof(allocations));
	allocation_failure = -1;
	prepare_error = open_error = mutex_error = mode = 0;
	hold_deinit = deinit_entered = deinit_released = 0;
	phase_limit = SIZE_MAX;
	last_tx_len = last_rx_len = transmitted_size = 0;
	atomic_store(&local_irq_model, 0);
	pthread_mutex_unlock(&control);
}

void async_i2c_probe_stats(struct async_i2c_stats *out)
{
	pthread_mutex_lock(&control);
	*out = stats;
	pthread_mutex_unlock(&control);
}

void async_i2c_probe_fail_alloc(int after)
{
	pthread_mutex_lock(&control);
	allocation_failure = after;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_prepare_error(int error)
{
	pthread_mutex_lock(&control);
	prepare_error = error;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_open_error(int error)
{
	pthread_mutex_lock(&control);
	open_error = error;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_mode(int value)
{
	pthread_mutex_lock(&control);
	mode = value;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_limit(size_t words)
{
	pthread_mutex_lock(&control);
	phase_limit = words;
	pthread_mutex_unlock(&control);
}

int async_i2c_test_mutex_init(ove_mutex_t *mutex, ove_mutex_storage_t *storage)
{
	pthread_mutex_lock(&control);
	int error = mutex_error;
	pthread_mutex_unlock(&control);
	if (error)
		return error;
	int result = ove_mutex_init(mutex, storage);
	if (result == OVE_OK) {
		pthread_mutex_lock(&control);
		++stats.mutex_inits;
		pthread_mutex_unlock(&control);
	}
	return result;
}
void async_i2c_test_mutex_deinit(ove_mutex_t mutex)
{
	pthread_mutex_lock(&control);
	if (hold_deinit) {
		deinit_entered = 1;
		pthread_cond_broadcast(&gate_cv);
		while (!deinit_released)
			pthread_cond_wait(&gate_cv, &control);
	}
	pthread_mutex_unlock(&control);
	ove_mutex_deinit(mutex);
	pthread_mutex_lock(&control);
	++stats.mutex_deinits;
	pthread_mutex_unlock(&control);
}

void *async_i2c_test_malloc(size_t bytes)
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

void async_i2c_test_free(void *pointer)
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
	/* Normal lifecycle controls exercise real reclamation. */
	free(pointer);
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *config)
{
	pthread_mutex_lock(&control);
	int error = open_error;
	configured_handle = (uintptr_t)i2c;
	last_instance = config->instance;
	last_speed = i2c->speed_hz;
	if (!error)
		++stats.opens;
	pthread_mutex_unlock(&control);
	if (!error)
		i2c->fd = 1;
	return error;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	pthread_mutex_lock(&control);
	++stats.closes;
	pthread_mutex_unlock(&control);
	i2c->fd = -1;
}

int ove_hal_i2c_async_prepare(void)
{
	pthread_mutex_lock(&control);
	++stats.prepares;
	int error = prepare_error;
	pthread_mutex_unlock(&control);
	return error;
}
int ove_hal_i2c_validate_transfer(ove_i2c_t i2c, size_t tx, size_t rx, int asynchronous)
{
	(void)i2c;
	(void)asynchronous;
	pthread_mutex_lock(&control);
	int result = tx > phase_limit || rx > phase_limit ? OVE_ERR_INVALID_PARAM : OVE_OK;
	pthread_mutex_unlock(&control);
	return result;
}
int ove_hal_i2c_write(ove_i2c_t h, uint16_t a, const void *p, size_t n, uint64_t t)
{
	(void)h;
	(void)a;
	(void)p;
	(void)n;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_read(ove_i2c_t h, uint16_t a, void *p, size_t n, uint64_t t)
{
	(void)h;
	(void)a;
	(void)p;
	(void)n;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_write_read(ove_i2c_t h, uint16_t a, const void *p, size_t pn, void *r, size_t rn,
			   uint64_t t)
{
	(void)h;
	(void)a;
	(void)p;
	(void)pn;
	(void)r;
	(void)rn;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_i2c_async_retire(ove_i2c_t i2c)
{
	ove_i2c_async_retired(i2c);
}

static uintptr_t watched_tx, watched_rx;
static size_t expected_tx, expected_rx;
static int watching, released;
void async_i2c_probe_watch(uintptr_t tx, size_t tn, uintptr_t rx, size_t rn)
{
	pthread_mutex_lock(&control);
	watched_tx = tx;
	watched_rx = rx;
	expected_tx = tn;
	expected_rx = rn;
	watching = 1;
	released = 0;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_release_app(void)
{
	pthread_mutex_lock(&control);
	released = 1;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_clear_watch(void)
{
	pthread_mutex_lock(&control);
	watching = released = 0;
	pthread_mutex_unlock(&control);
}
void async_i2c_probe_complete(int result)
{
	pthread_mutex_lock(&control);
	unsigned slot;
	for (slot = 0; slot < 16 && !jobs[slot].i2c; ++slot) {
	}
	assert(slot < 16);
	struct async_i2c_job job = jobs[slot];
	memset(&jobs[slot], 0, sizeof(jobs[slot]));
	int bad_tx = released && job.tx_len && job.tx >= watched_tx &&
		     job.tx - watched_tx < expected_tx;
	int bad_rx = released && job.rx_len && job.rx >= watched_rx &&
		     job.rx - watched_rx < expected_rx;
	if (bad_tx || bad_rx)
		++stats.invalid;
	transmitted_size = job.tx_len < sizeof(transmitted) ? job.tx_len : sizeof(transmitted);
	if (job.tx_len && !bad_tx) {
		memcpy(transmitted, (const void *)job.tx, transmitted_size);
		transmitted_tail = ((const unsigned char *)job.tx)[job.tx_len - 1];
	} else {
		transmitted_size = 0;
		transmitted_tail = 0;
	}
	if (job.rx_len && !bad_rx)
		memset((void *)job.rx, result == OVE_OK ? 0x5a : 0xee, job.rx_len);
	((ove_i2c_t)job.i2c)->pending_tx = NULL;
	((ove_i2c_t)job.i2c)->pending_rx = NULL;
	((ove_i2c_t)job.i2c)->pending_tx_len = ((ove_i2c_t)job.i2c)->pending_rx_len = 0;
	++stats.completions;
	pthread_mutex_unlock(&control);
	/* The cached object is unlinked; no controller access follows notification. */
	ove_i2c_async_complete((ove_i2c_t)job.i2c, result);
}

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t address, const void *tx, size_t tn,
				 void *rx, size_t rn)
{
	pthread_mutex_lock(&control);
	++stats.submissions;
	int behavior = mode;
	if ((watching && (tn != expected_tx || rn != expected_rx)) || (!tx && tn) || (!rx && rn)) {
		++stats.invalid;
		pthread_mutex_unlock(&control);
		return OVE_ERR_INVALID_PARAM;
	}
	if (behavior == 2) {
		pthread_mutex_unlock(&control);
		return OVE_ERR_IO;
	}
	unsigned slot;
	for (slot = 0; slot < 16 && jobs[slot].i2c; ++slot) {
	}
	assert(slot < 16);
	jobs[slot] = (struct async_i2c_job){(uintptr_t)i2c, (uintptr_t)tx, (uintptr_t)rx, tn, rn,
					    address};
	last_tx_len = tn;
	last_rx_len = rn;
	i2c->pending_tx = tx;
	i2c->pending_rx = rx;
	i2c->pending_tx_len = tn;
	i2c->pending_rx_len = rn;
	i2c->pending_addr = address;
	pthread_mutex_unlock(&control);
	if (behavior == 1)
		async_i2c_probe_complete(OVE_OK);
	return OVE_OK;
}

size_t async_i2c_probe_tx_len(void)
{
	pthread_mutex_lock(&control);
	size_t value = last_tx_len;
	pthread_mutex_unlock(&control);
	return value;
}
size_t async_i2c_probe_rx_len(void)
{
	pthread_mutex_lock(&control);
	size_t value = last_rx_len;
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_i2c_probe_tx_byte(size_t index)
{
	pthread_mutex_lock(&control);
	assert(index < transmitted_size);
	unsigned value = transmitted[index];
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_i2c_probe_tx_tail(void)
{
	pthread_mutex_lock(&control);
	unsigned value = transmitted_tail;
	pthread_mutex_unlock(&control);
	return value;
}
unsigned async_i2c_probe_config(unsigned field)
{
	pthread_mutex_lock(&control);
	unsigned value = field ? last_speed : last_instance;
	pthread_mutex_unlock(&control);
	return value;
}
uintptr_t async_i2c_probe_pending_tx(void)
{
	pthread_mutex_lock(&control);
	uintptr_t value = 0;
	for (unsigned i = 0; i < 16; ++i)
		if (jobs[i].i2c) {
			value = jobs[i].tx;
			break;
		}
	pthread_mutex_unlock(&control);
	return value;
}
uintptr_t async_i2c_probe_pending_rx(void)
{
	pthread_mutex_lock(&control);
	uintptr_t value = 0;
	for (unsigned i = 0; i < 16; ++i)
		if (jobs[i].i2c) {
			value = jobs[i].rx;
			break;
		}
	pthread_mutex_unlock(&control);
	return value;
}

void async_i2c_probe_mutex_error(int error)
{
	pthread_mutex_lock(&control);
	mutex_error = error;
	pthread_mutex_unlock(&control);
}

void async_i2c_probe_hold_deinit(void)
{
	pthread_mutex_lock(&control);
	hold_deinit = 1;
	deinit_entered = deinit_released = 0;
	pthread_mutex_unlock(&control);
}
int async_i2c_probe_wait_deinit(void)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	pthread_mutex_lock(&control);
	while (!deinit_entered)
		if (pthread_cond_timedwait(&gate_cv, &control, &deadline))
			break;
	int result = deinit_entered;
	pthread_mutex_unlock(&control);
	return result;
}
void async_i2c_probe_release_deinit(void)
{
	pthread_mutex_lock(&control);
	deinit_released = 1;
	hold_deinit = 0;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&control);
}
int async_i2c_probe_check(uint16_t address, size_t tx, size_t rx)
{
	return ove_i2c_binding_check_lengths((ove_i2c_t)configured_handle, address, tx, rx);
}
unsigned async_i2c_probe_pending_count(void)
{
	pthread_mutex_lock(&control);
	unsigned count = 0;
	for (unsigned i = 0; i < 16; ++i)
		if (jobs[i].i2c)
			++count;
	pthread_mutex_unlock(&control);
	return count;
}
