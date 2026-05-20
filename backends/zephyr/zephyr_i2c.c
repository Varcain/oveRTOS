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

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	const struct device *dev = instance_to_dev(cfg->instance);

	if (dev == NULL || !device_is_ready(dev))
		return OVE_ERR_INVALID_PARAM;

	i2c->dev = dev;

	uint32_t i2c_cfg = I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed_to_zephyr(i2c->speed_hz));
	int ret = i2c_configure(dev, i2c_cfg);
	if (ret != 0)
		return OVE_ERR_NOT_SUPPORTED;

	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	i2c->dev = NULL;
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	(void)timeout_ns;

	int ret = i2c_write(i2c->dev, data, len, addr);
	if (ret == -EIO)
		return OVE_ERR_BUS_NACK;
	return (ret == 0) ? OVE_OK : OVE_ERR_BUS_ERROR;
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	(void)timeout_ns;

	int ret = i2c_read(i2c->dev, buf, len, addr);
	if (ret == -EIO)
		return OVE_ERR_BUS_NACK;
	return (ret == 0) ? OVE_OK : OVE_ERR_BUS_ERROR;
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	(void)timeout_ns;

	int ret = i2c_write_read(i2c->dev, addr, tx, tx_len, rx, rx_len);
	if (ret == -EIO)
		return OVE_ERR_BUS_NACK;
	return (ret == 0) ? OVE_OK : OVE_ERR_BUS_ERROR;
}

#ifdef CONFIG_OVE_ASYNC

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

	ove_i2c_async_complete(i2c, result);
}

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	i2c->pending_addr = addr;
	i2c->pending_tx = tx;
	i2c->pending_tx_len = tx_len;
	i2c->pending_rx = rx;
	i2c->pending_rx_len = rx_len;
	k_work_init(&i2c->async_work, zephyr_i2c_async_work_handler);
	int ret = k_work_submit(&i2c->async_work);
	if (ret < 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
