/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2C

#include "ove/i2c.h"
#include "ove/hal/hal_i2c.h"
#include "ove/sync.h"
#include "ove_backend_common.h"

#include <string.h>

/* Speed enum → Hz for storage; 0 for a value outside the enum */
static uint32_t speed_to_hz(ove_i2c_speed_t speed)
{
	switch (speed) {
	case OVE_I2C_SPEED_STANDARD:
		return 100000;
	case OVE_I2C_SPEED_FAST:
		return 400000;
	case OVE_I2C_SPEED_FAST_PLUS:
		return 1000000;
	default:
		return 0;
	}
}

static int i2c_validate_transfer(ove_i2c_t i2c, uint16_t addr, size_t tx_len, size_t rx_len,
				 int asynchronous)
{
	if (i2c == NULL || addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	return ove_hal_i2c_validate_transfer(i2c, tx_len, rx_len, asynchronous);
}

#ifdef CONFIG_OVE_ASYNC
enum {
	I2C_ACTIVE = 1,
	I2C_SUBMITTING = 2,
	I2C_RESULT = 4,
	I2C_CLOSING = 8,
	I2C_DESTROY = 16,
	I2C_COMPLETING = 32,
};
#endif

/* ── Lifecycle ───────────────────────────────────────────────────── */

int ove_i2c_init(ove_i2c_t *i2c, ove_i2c_storage_t *storage, const struct ove_i2c_cfg *cfg)
{
	int ret;

	if (i2c == NULL || storage == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;

	uint32_t speed_hz = speed_to_hz(cfg->speed);
	if (speed_hz == 0)
		return OVE_ERR_INVALID_PARAM;

	storage->instance = cfg->instance;
	storage->speed_hz = speed_hz;
#ifdef CONFIG_OVE_ASYNC
	storage->async_busy = 0;
	storage->retire_next = NULL;
	storage->async_result = OVE_OK;
	storage->pending_cb = NULL;
	storage->pending_ud = NULL;
#endif
	*i2c = storage;

	ret = ove_mutex_init(&storage->bus_mtx, &storage->bus_mtx_storage);
	if (ret != OVE_OK)
		return ret;

	ret = ove_hal_i2c_open(*i2c, cfg);
	if (ret != OVE_OK) {
		ove_mutex_deinit(storage->bus_mtx);
		return ret;
	}

	return OVE_OK;
}

static void i2c_deinit_now(ove_i2c_t i2c)
{
	ove_hal_i2c_close(i2c);
	ove_mutex_deinit(i2c->bus_mtx);
}

#ifdef CONFIG_OVE_ASYNC
static int i2c_request_close(ove_i2c_t i2c, int destroy)
{
	int previous = __atomic_fetch_or(
		&i2c->async_busy, I2C_CLOSING | (destroy ? I2C_DESTROY : 0), __ATOMIC_ACQ_REL);
	return previous & (I2C_ACTIVE | I2C_CLOSING);
}

static void i2c_finish_close(ove_i2c_t i2c, int flags)
{
	if (flags & I2C_CLOSING) {
		i2c_deinit_now(i2c);
#ifdef OVE_HEAP_I2C
		if (flags & I2C_DESTROY)
			OVE_BACKEND_FREE(i2c);
#endif
	}
}
#endif

void ove_i2c_deinit(ove_i2c_t i2c)
{
	if (i2c == NULL)
		return;
#ifdef CONFIG_OVE_ASYNC
	if (i2c_request_close(i2c, 0))
		return;
#endif
	i2c_deinit_now(i2c);
}

#ifdef OVE_HEAP_I2C
int ove_i2c_create(ove_i2c_t *i2c, const struct ove_i2c_cfg *cfg)
{
	ove_i2c_storage_t *storage;

	if (i2c == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;

	storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (storage == NULL)
		return OVE_ERR_NO_MEMORY;

	memset(storage, 0, sizeof(*storage));

	int ret = ove_i2c_init(i2c, storage, cfg);
	if (ret != OVE_OK) {
		OVE_BACKEND_FREE(storage);
		return ret;
	}
	return OVE_OK;
}

void ove_i2c_destroy(ove_i2c_t i2c)
{
	if (i2c == NULL)
		return;
#ifdef CONFIG_OVE_ASYNC
	if (i2c_request_close(i2c, 1))
		return;
#endif
	i2c_deinit_now(i2c);
	OVE_BACKEND_FREE(i2c);
}
#endif /* OVE_HEAP_I2C */

/* ── Operations ──────────────────────────────────────────────────── */

int ove_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len, uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL || (data == NULL && len > 0))
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, len, 0, 0);
	if (ret != OVE_OK)
		return ret;

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_write(i2c, addr, data, len, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

int ove_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL || buf == NULL || len == 0)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, 0, len, 0);
	if (ret != OVE_OK)
		return ret;

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_read(i2c, addr, buf, len, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

int ove_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
		       size_t rx_len, uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL || (tx == NULL && tx_len > 0) || rx == NULL || rx_len == 0)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, tx_len, rx_len, 0);
	if (ret != OVE_OK)
		return ret;

	/* Nothing to write is a plain read on every engine, rather than a
	 * zero-length write each backend treats its own way. */
	OVE_LOCK_INFINITE(i2c->bus_mtx);
	if (tx_len == 0)
		ret = ove_hal_i2c_read(i2c, addr, rx, rx_len, timeout_ns);
	else
		ret = ove_hal_i2c_write_read(i2c, addr, tx, tx_len, rx, rx_len, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

/* ── Register convenience ────────────────────────────────────────── */

int ove_i2c_reg_write(ove_i2c_t i2c, uint16_t addr, uint8_t reg, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	uint8_t buf[1 + OVE_I2C_REG_WRITE_MAX];
	int ret;

	if (i2c == NULL || (data == NULL && len > 0))
		return OVE_ERR_INVALID_PARAM;
	if (len > sizeof(buf) - 1)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, len + 1, 0, 0);
	if (ret != OVE_OK)
		return ret;

	buf[0] = reg;
	if (len > 0)
		memcpy(&buf[1], data, len);

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_write(i2c, addr, buf, 1 + len, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

int ove_i2c_reg_read(ove_i2c_t i2c, uint16_t addr, uint8_t reg, void *buf, size_t len,
		     uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL || buf == NULL || len == 0)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, 1, len, 0);
	if (ret != OVE_OK)
		return ret;

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_write_read(i2c, addr, &reg, 1, buf, len, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

/* ── Bus probe ───────────────────────────────────────────────────── */

int ove_i2c_probe(ove_i2c_t i2c, uint16_t addr, uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, 0, 0, 0);
	if (ret != OVE_OK)
		return ret;

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_write(i2c, addr, NULL, 0, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

#ifdef CONFIG_OVE_ASYNC

int ove_i2c_async_prepare(void)
{
	return ove_hal_i2c_async_prepare();
}

int ove_i2c_binding_check_lengths(ove_i2c_t i2c, uint16_t addr, size_t tx_len, size_t rx_len)
{
	if (tx_len == 0 && rx_len == 0)
		return OVE_ERR_INVALID_PARAM;
	return i2c_validate_transfer(i2c, addr, tx_len, rx_len, 1);
}

#ifdef OVE_HEAP_I2C
void *ove_i2c_binding_alloc(size_t size, size_t alignment)
{
	if (!size || !alignment || (alignment & (alignment - 1)))
		return NULL;
	if (alignment - 1 > (size_t)PTRDIFF_MAX - sizeof(void *))
		return NULL;
	size_t padding = sizeof(void *) + alignment - 1;
	if (size > (size_t)PTRDIFF_MAX - padding)
		return NULL;
	void *allocation = OVE_BACKEND_MALLOC(size + padding);
	if (!allocation)
		return NULL;
	uintptr_t address = (uintptr_t)allocation + sizeof(void *);
	address = (address + alignment - 1) & ~(uintptr_t)(alignment - 1);
	void *pointer = (void *)address;
	memcpy((uint8_t *)pointer - sizeof(allocation), &allocation, sizeof(allocation));
	return pointer;
}

void ove_i2c_binding_free(void *pointer)
{
	if (pointer) {
		void *allocation;
		memcpy(&allocation, (uint8_t *)pointer - sizeof(allocation), sizeof(allocation));
		OVE_BACKEND_FREE(allocation);
	}
}
#endif

static void i2c_clear_pending(ove_i2c_t i2c)
{
	i2c->pending_cb = NULL;
	i2c->pending_ud = NULL;
}

int ove_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			     size_t rx_len, ove_dma_complete_cb cb, void *user_data)
{
	int ret;

	if (i2c == NULL || cb == NULL)
		return OVE_ERR_INVALID_PARAM;
	if (tx == NULL && tx_len > 0)
		return OVE_ERR_INVALID_PARAM;
	if (rx == NULL && rx_len > 0)
		return OVE_ERR_INVALID_PARAM;
	if (tx_len == 0 && rx_len == 0)
		return OVE_ERR_INVALID_PARAM;

	ret = i2c_validate_transfer(i2c, addr, tx_len, rx_len, 1);
	if (ret != OVE_OK)
		return ret;

	ret = ove_i2c_async_prepare();
	if (ret != OVE_OK)
		return ret;
	int idle = 0;
	if (!__atomic_compare_exchange_n(&i2c->async_busy, &idle, I2C_ACTIVE | I2C_SUBMITTING, 0,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return OVE_ERR_BUS_BUSY;
	i2c->pending_cb = cb;
	i2c->pending_ud = user_data;

	ret = ove_hal_i2c_write_read_async(i2c, addr, tx, tx_len, rx, rx_len);
	if (ret != OVE_OK) {
		/* Rejection retains no provider state and produces no callback. */
		i2c_clear_pending(i2c);
		int flags = __atomic_fetch_and(&i2c->async_busy, I2C_CLOSING | I2C_DESTROY,
					       __ATOMIC_ACQ_REL);
		i2c_finish_close(i2c, flags);
		return ret;
	}
	/* Paired with result publication: the participant observing the other
	 * event owns the sole retirement request. This is the submitter's last
	 * controller access before handing off to the provider. */
	int flags = __atomic_fetch_and(&i2c->async_busy, ~I2C_SUBMITTING, __ATOMIC_ACQ_REL);
	if (flags & I2C_RESULT)
		ove_hal_i2c_async_retire(i2c);
	return ret;
}

void ove_i2c_async_complete(ove_i2c_t i2c, int result)
{
	int flags = __atomic_load_n(&i2c->async_busy, __ATOMIC_ACQUIRE);
	do {
		if (!(flags & I2C_ACTIVE) || (flags & I2C_COMPLETING))
			return;
	} while (!__atomic_compare_exchange_n(&i2c->async_busy, &flags, flags | I2C_COMPLETING, 0,
					      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
	i2c->async_result = result;
	flags = __atomic_fetch_or(&i2c->async_busy, I2C_RESULT, __ATOMIC_ACQ_REL);
	if (!(flags & I2C_SUBMITTING))
		ove_hal_i2c_async_retire(i2c);
}

void ove_i2c_async_retired(ove_i2c_t i2c)
{
	/* Provider queues have already unlinked this object. All arguments and
	 * cleanup choices must be cached before dropping its final active state. */
	ove_dma_complete_cb cb = i2c->pending_cb;
	void *user_data = i2c->pending_ud;
	int result = i2c->async_result;
	i2c_clear_pending(i2c);
	int flags =
		__atomic_fetch_and(&i2c->async_busy, I2C_CLOSING | I2C_DESTROY, __ATOMIC_ACQ_REL);
	i2c_finish_close(i2c, flags);
	if (cb != NULL)
		cb(result, user_data);
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
