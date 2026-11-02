/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove_config.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
enum {
	ALLOC,
	FREE,
	INIT,
	DEINIT,
	OPEN,
	CLOSE,
	HAL,
	LOCK,
	UNLOCK,
	INVALID,
	INIT_ENTRY,
	DESTROY,
	CONSOLE,
	STAT_COUNT
};
static unsigned stats[STAT_COUNT], failure;
static int gate, entered, released, quarantine;
static char output[2048];
static size_t output_len;
struct record {
	uintptr_t pointer;
	int live, physical;
};
static struct record allocations[64], storage_slots[64], mutex_slots[64];
static struct record *slot(struct record *records, uintptr_t pointer)
{
	for (unsigned i = 0; i < 64; ++i)
		if (records[i].pointer == pointer)
			return &records[i];
	for (unsigned i = 0; i < 64; ++i)
		if (!records[i].live && !records[i].physical) {
			records[i].pointer = pointer;
			return &records[i];
		}
	abort();
}
#ifdef CONFIG_OVE_I2C
static void reset_io(void);
#endif
void i2c_test_reset(void)
{
	pthread_mutex_lock(&state_lock);
	memset(stats, 0, sizeof(stats));
	failure = 0;
	gate = entered = released = quarantine = 0;
	output_len = 0;
	output[0] = 0;
#ifdef CONFIG_OVE_I2C
	reset_io();
#endif
	pthread_mutex_unlock(&state_lock);
}
unsigned i2c_test_stat(unsigned index)
{
	pthread_mutex_lock(&state_lock);
	unsigned value = index < STAT_COUNT ? stats[index] : 0;
	pthread_mutex_unlock(&state_lock);
	return value;
}
void i2c_test_fail(unsigned stage)
{
	failure = stage;
}
void i2c_test_quarantine(int enabled)
{
	quarantine = enabled;
}
void i2c_test_release_quarantine(void)
{
	for (unsigned i = 0; i < 64; ++i)
		if (allocations[i].physical && !allocations[i].live) {
			free((void *)allocations[i].pointer);
			allocations[i].physical = 0;
		}
}
const char *i2c_test_output(void)
{
	return output;
}
#ifdef CONFIG_OVE_CONSOLE
void ove_console_write(const char *data, unsigned int len)
{
	pthread_mutex_lock(&state_lock);
	++stats[CONSOLE];
	if (len <= sizeof(output) - 1 - output_len) {
		memcpy(output + output_len, data, len);
		output_len += len;
		output[output_len] = 0;
	} else
		++stats[INVALID];
	pthread_mutex_unlock(&state_lock);
}
#endif

#ifdef CONFIG_OVE_I2C
#include "ove/hal/hal_i2c.h"
#include "ove/sync.h"
static _Thread_local ove_mutex_t held;
static unsigned kind, address;
static size_t tx_len, rx_len;
static uint64_t timeout;
static uintptr_t tx_pointer, rx_pointer, bound_tx, bound_rx;
static size_t bound_tx_len, bound_rx_len;
static unsigned char payload[256];
int i2c_actual_init(ove_i2c_t *, ove_i2c_storage_t *, const struct ove_i2c_cfg *);
void i2c_actual_deinit(ove_i2c_t);
#ifdef OVE_HEAP_I2C
void i2c_actual_destroy(ove_i2c_t);
#endif
static unsigned configured_instance, configured_speed;
static void reset_io(void)
{
	kind = address = 0;
	tx_len = rx_len = 0;
	timeout = tx_pointer = rx_pointer = 0;
	bound_tx = bound_rx = bound_tx_len = bound_rx_len = 0;
	memset(payload, 0, sizeof(payload));
}

void *i2c_test_malloc(size_t size)
{
	pthread_mutex_lock(&state_lock);
	++stats[ALLOC];
	int reject = failure == 1;
	pthread_mutex_unlock(&state_lock);
	if (reject)
		return NULL;
	void *pointer = malloc(size);
	if (pointer) {
		pthread_mutex_lock(&state_lock);
		struct record *r = slot(allocations, (uintptr_t)pointer);
		r->live = r->physical = 1;
		pthread_mutex_unlock(&state_lock);
	}
	return pointer;
}
void i2c_test_free(void *pointer)
{
	pthread_mutex_lock(&state_lock);
	struct record *r = slot(allocations, (uintptr_t)pointer);
	if (!r->live) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return;
	}
	++stats[FREE];
	r->live = 0;
	int retained = quarantine;
	if (!retained)
		r->physical = 0;
	pthread_mutex_unlock(&state_lock);
	if (!retained)
		free(pointer);
}
int ove_i2c_init(ove_i2c_t *handle, ove_i2c_storage_t *storage, const struct ove_i2c_cfg *config)
{
	pthread_mutex_lock(&state_lock);
	++stats[INIT_ENTRY];
	struct record *r = slot(storage_slots, (uintptr_t)storage);
	/* Stop premature reuse before it could reinitialize a live pthread mutex. */
	if (r->live) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return OVE_ERR_BUSY;
	}
	r->live = 1;
	pthread_mutex_unlock(&state_lock);
	int rc = i2c_actual_init(handle, storage, config);
	if (rc != OVE_OK) {
		pthread_mutex_lock(&state_lock);
		r->live = 0;
		pthread_mutex_unlock(&state_lock);
	}
	return rc;
}
void i2c_test_hold_deinit(void)
{
	pthread_mutex_lock(&state_lock);
	gate = 1;
	entered = released = 0;
	pthread_mutex_unlock(&state_lock);
}
int i2c_test_wait_deinit(void)
{
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 2;
	pthread_mutex_lock(&state_lock);
	while (!entered)
		if (pthread_cond_timedwait(&gate_cv, &state_lock, &until) != 0)
			break;
	int value = entered;
	pthread_mutex_unlock(&state_lock);
	return value;
}
void i2c_test_finish_deinit(void)
{
	pthread_mutex_lock(&state_lock);
	released = 1;
	pthread_cond_broadcast(&gate_cv);
	pthread_mutex_unlock(&state_lock);
}
void ove_i2c_deinit(ove_i2c_t handle)
{
	if (!handle)
		return;
	pthread_mutex_lock(&state_lock);
	struct record *r = slot(storage_slots, (uintptr_t)handle);
	if (!r->live) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return;
	}
	if (gate) {
		entered = 1;
		pthread_cond_broadcast(&gate_cv);
		while (!released)
			pthread_cond_wait(&gate_cv, &state_lock);
	}
	pthread_mutex_unlock(&state_lock);
	i2c_actual_deinit(handle);
	pthread_mutex_lock(&state_lock);
	r->live = 0;
	pthread_mutex_unlock(&state_lock);
}
#ifdef OVE_HEAP_I2C
void ove_i2c_destroy(ove_i2c_t handle)
{
	if (!handle)
		return;
	pthread_mutex_lock(&state_lock);
	++stats[DESTROY];
	int live = slot(allocations, (uintptr_t)handle)->live;
	if (!live)
		++stats[INVALID];
	pthread_mutex_unlock(&state_lock);
	if (live)
		i2c_actual_destroy(handle);
}
#endif
int i2c_test_mutex_init(ove_mutex_t *handle, ove_mutex_storage_t *storage)
{
	pthread_mutex_lock(&state_lock);
	++stats[INIT];
	struct record *r = slot(mutex_slots, (uintptr_t)storage);
	if (r->live) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return OVE_ERR_BUSY;
	}
	int reject = failure == 2;
	if (!reject)
		r->physical = 1;
	pthread_mutex_unlock(&state_lock);
	if (reject)
		return OVE_ERR_NO_MEMORY;
	int rc = ove_mutex_init(handle, storage);
	pthread_mutex_lock(&state_lock);
	r->live = rc == OVE_OK;
	r->physical = 0;
	pthread_mutex_unlock(&state_lock);
	return rc;
}
void i2c_test_mutex_deinit(ove_mutex_t handle)
{
	pthread_mutex_lock(&state_lock);
	struct record *r = slot(mutex_slots, (uintptr_t)handle);
	if (!r->live) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return;
	}
	++stats[DEINIT];
	r->live = 0;
	pthread_mutex_unlock(&state_lock);
	ove_mutex_deinit(handle);
}
int i2c_test_lock(ove_mutex_t handle, uint64_t ns)
{
	int rc = ove_mutex_lock(handle, ns);
	pthread_mutex_lock(&state_lock);
	if (held)
		++stats[INVALID];
	if (rc == OVE_OK) {
		held = handle;
		++stats[LOCK];
	}
	pthread_mutex_unlock(&state_lock);
	return rc;
}
void i2c_test_unlock(ove_mutex_t handle)
{
	pthread_mutex_lock(&state_lock);
	if (held != handle)
		++stats[INVALID];
	held = NULL;
	++stats[UNLOCK];
	pthread_mutex_unlock(&state_lock);
	ove_mutex_unlock(handle);
}
int ove_hal_i2c_open(ove_i2c_t handle, const struct ove_i2c_cfg *config)
{
	pthread_mutex_lock(&state_lock);
	++stats[OPEN];
	configured_instance = config->instance;
	configured_speed = handle->speed_hz;
	int reject = failure == 3;
	pthread_mutex_unlock(&state_lock);
	if (reject)
		return OVE_ERR_IO;
	handle->fd = 1;
	return OVE_OK;
}
void ove_hal_i2c_close(ove_i2c_t handle)
{
	pthread_mutex_lock(&state_lock);
	++stats[CLOSE];
	pthread_mutex_unlock(&state_lock);
	handle->fd = -1;
}
int ove_hal_i2c_validate_transfer(ove_i2c_t handle, size_t tx, size_t rx, int asynchronous)
{
	(void)handle;
	(void)asynchronous;
	return tx > UINT16_MAX || rx > UINT16_MAX ? OVE_ERR_INVALID_PARAM : OVE_OK;
}
void i2c_test_bounds(const void *tx, size_t tx_size, void *rx, size_t rx_size)
{
	bound_tx = (uintptr_t)tx;
	bound_tx_len = tx_size;
	bound_rx = (uintptr_t)rx;
	bound_rx_len = rx_size;
}
static int transfer(ove_i2c_t handle, unsigned operation, uint16_t addr, const void *tx, size_t wn,
		    void *rx, size_t rn, uint64_t ns)
{
	pthread_mutex_lock(&state_lock);
	++stats[HAL];
	kind = operation;
	address = addr;
	tx_len = wn;
	rx_len = rn;
	timeout = ns;
	tx_pointer = (uintptr_t)tx;
	rx_pointer = (uintptr_t)rx;
	if (held != handle->bus_mtx)
		++stats[INVALID];
	if ((tx && (uintptr_t)tx == bound_tx && wn > bound_tx_len) ||
	    (rx && (uintptr_t)rx == bound_rx && rn > bound_rx_len)) {
		++stats[INVALID];
		pthread_mutex_unlock(&state_lock);
		return OVE_ERR_INVALID_PARAM;
	}
	if (tx && wn)
		memcpy(payload, tx, wn < sizeof(payload) ? wn : sizeof(payload));
	int reject = failure == 4;
	pthread_mutex_unlock(&state_lock);
	if (reject)
		return OVE_ERR_BUS_NACK;
	if (rx)
		memset(rx, 0x5a, rn);
	return OVE_OK;
}
int ove_hal_i2c_write(ove_i2c_t h, uint16_t a, const void *p, size_t n, uint64_t t)
{
	return transfer(h, 1, a, p, n, NULL, 0, t);
}
int ove_hal_i2c_read(ove_i2c_t h, uint16_t a, void *p, size_t n, uint64_t t)
{
	return transfer(h, 2, a, NULL, 0, p, n, t);
}
int ove_hal_i2c_write_read(ove_i2c_t h, uint16_t a, const void *w, size_t wn, void *r, size_t rn,
			   uint64_t t)
{
	return transfer(h, 3, a, w, wn, r, rn, t);
}
uint64_t i2c_test_last(unsigned field)
{
	switch (field) {
	case 0:
		return kind;
	case 1:
		return address;
	case 2:
		return tx_len;
	case 3:
		return rx_len;
	case 4:
		return timeout;
	case 5:
		return tx_pointer;
	case 6:
		return rx_pointer;
	case 7:
		return configured_instance;
	case 8:
		return configured_speed;
	default:
		return 0;
	}
}
unsigned i2c_test_byte(unsigned index)
{
	return index < sizeof(payload) ? payload[index] : 0;
}
static ove_i2c_storage_t external_storage;
ove_i2c_t i2c_test_raw_open(void)
{
	ove_i2c_t handle = NULL;
	struct ove_i2c_cfg config = {.speed = OVE_I2C_SPEED_STANDARD};
	if (ove_i2c_init(&handle, &external_storage, &config) != OVE_OK)
		return NULL;
	return handle;
}
void i2c_test_raw_close(ove_i2c_t handle)
{
	ove_i2c_deinit(handle);
}
size_t i2c_test_native_size(void)
{
	return sizeof(ove_i2c_storage_t);
}
size_t i2c_test_native_align(void)
{
	return _Alignof(ove_i2c_storage_t);
}
#endif
