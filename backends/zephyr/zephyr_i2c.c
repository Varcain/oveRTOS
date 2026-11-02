/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2C

#include "ove/hal/hal_i2c.h"
#include "ove_backend_common.h"
#include <zephyr/drivers/i2c.h>

#ifdef CONFIG_OVE_ASYNC
static void zephyr_i2c_async_work_handler(struct k_work *work);
#endif

static const struct device *instance_to_dev(unsigned int instance)
{
	switch (instance) {
	case 0:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c1));
	case 1:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c2));
	case 2:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c3));
	case 3:
		return DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c4));
	default:
		return NULL;
	}
}

static uint32_t speed_to_zephyr(uint32_t speed_hz)
{
	if (speed_hz >= 1000000)
		return I2C_SPEED_FAST_PLUS;
	if (speed_hz >= 400000)
		return I2C_SPEED_FAST;
	return I2C_SPEED_STANDARD;
}

/* Translate a negative errno from a Zephyr I2C transfer.  The STM32 driver
 * reports NACK, arbitration loss and timeout all as -EIO, so -EIO stays a
 * NACK: the common case, and the one ove_i2c_probe relies on. */
static int i2c_errno_to_ove(int ret)
{
	switch (ret) {
	case 0:
		return OVE_OK;
	case -EIO:
	case -ENXIO:
		return OVE_ERR_BUS_NACK;
	case -ETIMEDOUT:
		return OVE_ERR_TIMEOUT;
	case -EBUSY:
		return OVE_ERR_BUS_BUSY;
	default:
		return OVE_ERR_BUS_ERROR;
	}
}

int ove_hal_i2c_validate_transfer(ove_i2c_t i2c, size_t tx_len, size_t rx_len, int asynchronous)
{
	(void)i2c;
	(void)asynchronous;
	if (tx_len > UINT32_MAX || rx_len > UINT32_MAX)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	const struct device *dev = instance_to_dev(cfg->instance);

	if (dev == NULL || !device_is_ready(dev))
		return OVE_ERR_INVALID_PARAM;

	i2c->dev = dev;

	uint32_t i2c_cfg = I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed_to_zephyr(i2c->speed_hz));
	int ret = i2c_configure(dev, i2c_cfg);
	if (ret != 0)
		return ove_errno_to_ove(-ret);

#ifdef CONFIG_OVE_ASYNC
	k_work_init(&i2c->async_work, zephyr_i2c_async_work_handler);
#endif

	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	i2c->dev = NULL;
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, len, 0, 0);
	if (validation != OVE_OK)
		return validation;

	(void)timeout_ns;

	int ret = i2c_write(i2c->dev, data, (uint32_t)len, addr);
	return i2c_errno_to_ove(ret);
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, 0, len, 0);
	if (validation != OVE_OK)
		return validation;

	(void)timeout_ns;

	int ret = i2c_read(i2c->dev, buf, (uint32_t)len, addr);
	return i2c_errno_to_ove(ret);
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, tx_len, rx_len, 0);
	if (validation != OVE_OK)
		return validation;

	(void)timeout_ns;

	int ret = i2c_write_read(i2c->dev, addr, tx, (uint32_t)tx_len, rx, (uint32_t)rx_len);
	return i2c_errno_to_ove(ret);
}

#ifdef CONFIG_OVE_ASYNC

static struct k_spinlock retire_lock;
static ove_i2c_t retire_head, retire_tail;
static int retire_initializing, retire_ready;
static void zephyr_i2c_retire_dispatch(struct k_work *work);
static K_WORK_DEFINE(retire_work, zephyr_i2c_retire_dispatch);

static void zephyr_i2c_retire_retry(struct k_timer *timer)
{
	(void)timer;
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	int pending = retire_head != NULL;
	k_spin_unlock(&retire_lock, key);
	if (pending) {
		int rc = k_work_submit(&retire_work);
		(void)rc;
	}
}
static K_TIMER_DEFINE(retire_retry, zephyr_i2c_retire_retry, NULL);

int ove_hal_i2c_async_prepare(void)
{
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	if (retire_ready || retire_initializing) {
		int rc = retire_ready ? OVE_OK : OVE_ERR_BUSY;
		k_spin_unlock(&retire_lock, key);
		return rc;
	}
	retire_initializing = 1;
	k_spin_unlock(&retire_lock, key);
	k_timer_start(&retire_retry, K_MSEC(10), K_MSEC(10));
	key = k_spin_lock(&retire_lock);
	retire_ready = 1;
	retire_initializing = 0;
	k_spin_unlock(&retire_lock, key);
	return OVE_OK;
}

void ove_hal_i2c_async_retire(ove_i2c_t i2c)
{
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	i2c->retire_next = NULL;
	if (retire_tail)
		retire_tail->retire_next = i2c;
	else
		retire_head = i2c;
	retire_tail = i2c;
	k_spin_unlock(&retire_lock, key);
	/* Same-queue dispatch runs only after the transfer work's kernel tail.
	 * Only this permanent work object survives in queued notifications. */
	int rc = k_work_submit(&retire_work);
	/* A plugged/draining system queue rejects notification. The stable
	 * periodic retry preserves progress after it becomes available again. */
	(void)rc;
}

static void zephyr_i2c_retire_dispatch(struct k_work *work)
{
	(void)work;
	for (unsigned i = 0; i < 4; ++i) {
		k_spinlock_key_t key = k_spin_lock(&retire_lock);
		ove_i2c_t i2c = retire_head;
		if (i2c) {
			retire_head = i2c->retire_next;
			if (!retire_head)
				retire_tail = NULL;
			i2c->retire_next = NULL;
		}
		k_spin_unlock(&retire_lock, key);
		if (!i2c)
			return;
		ove_i2c_async_retired(i2c);
	}
	k_spinlock_key_t key = k_spin_lock(&retire_lock);
	int more = retire_head != NULL;
	k_spin_unlock(&retire_lock, key);
	if (more) {
		int rc = k_work_submit(&retire_work);
		(void)rc;
	}
}

static void i2c_clear_transfer(ove_i2c_t i2c)
{
	i2c->pending_addr = 0;
	i2c->pending_tx = NULL;
	i2c->pending_tx_len = 0;
	i2c->pending_rx = NULL;
	i2c->pending_rx_len = 0;
}

static void zephyr_i2c_async_work_handler(struct k_work *work)
{
	struct ove_i2c *i2c = CONTAINER_OF(work, struct ove_i2c, async_work);
	int result;

	if (i2c->pending_tx_len > 0 && i2c->pending_rx_len > 0)
		result = ove_hal_i2c_write_read(i2c, i2c->pending_addr, i2c->pending_tx,
						i2c->pending_tx_len, i2c->pending_rx,
						i2c->pending_rx_len, OVE_WAIT_FOREVER);
	else if (i2c->pending_rx_len > 0)
		result = ove_hal_i2c_read(i2c, i2c->pending_addr, i2c->pending_rx,
					  i2c->pending_rx_len, OVE_WAIT_FOREVER);
	else
		result = ove_hal_i2c_write(i2c, i2c->pending_addr, i2c->pending_tx,
					   i2c->pending_tx_len, OVE_WAIT_FOREVER);

	i2c_clear_transfer(i2c);
	ove_i2c_async_complete(i2c, result);
}

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, tx_len, rx_len, 1);
	if (validation != OVE_OK)
		return validation;

	i2c->pending_addr = addr;
	i2c->pending_tx = tx;
	i2c->pending_tx_len = tx_len;
	i2c->pending_rx = rx;
	i2c->pending_rx_len = rx_len;
	int ret = k_work_submit(&i2c->async_work);
	if (ret < 0) {
		i2c_clear_transfer(i2c);
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
