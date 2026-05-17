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

/* Speed enum → Hz for storage */
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
		return 100000;
	}
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

int ove_i2c_init(ove_i2c_t *i2c, ove_i2c_storage_t *storage, const struct ove_i2c_cfg *cfg)
{
	int ret;

	if (i2c == NULL || storage == NULL || cfg == NULL)
		return OVE_ERR_INVALID_PARAM;

	storage->instance = cfg->instance;
	storage->speed_hz = speed_to_hz(cfg->speed);
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

void ove_i2c_deinit(ove_i2c_t i2c)
{
	if (i2c == NULL)
		return;
	ove_hal_i2c_close(i2c);
	ove_mutex_deinit(i2c->bus_mtx);
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
	ove_i2c_deinit(i2c);
	OVE_BACKEND_FREE(i2c);
}
#endif /* OVE_HEAP_I2C */

/* ── Operations ──────────────────────────────────────────────────── */

int ove_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len, uint64_t timeout_ns)
{
	int ret;

	if (i2c == NULL || (data == NULL && len > 0))
		return OVE_ERR_INVALID_PARAM;

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

	OVE_LOCK_INFINITE(i2c->bus_mtx);
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

	OVE_LOCK_INFINITE(i2c->bus_mtx);
	ret = ove_hal_i2c_write(i2c, addr, NULL, 0, timeout_ns);
	ove_mutex_unlock(i2c->bus_mtx);

	return ret;
}

#endif /* CONFIG_OVE_I2C */
