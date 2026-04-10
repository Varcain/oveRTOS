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

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <nuttx/i2c/i2c_master.h>

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	char path[32];

	snprintf(path, sizeof(path), "/dev/i2c%u", cfg->instance);

	int fd = open(path, O_RDWR);
	if (fd < 0)
		return OVE_ERR_INVALID_PARAM;

	i2c->fd = fd;
	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	if (i2c->fd >= 0) {
		close(i2c->fd);
		i2c->fd = -1;
	}
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr,
		      const void *data, size_t len,
		      uint32_t timeout_ms)
{
	(void)timeout_ms;

	struct i2c_msg_s msg = {
		.frequency = i2c->speed_hz,
		.addr      = addr,
		.flags     = 0,
		.buffer    = (uint8_t *)data,
		.length    = len,
	};
	struct i2c_transfer_s xfer = {
		.msgv  = &msg,
		.msgc  = 1,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER,
			(unsigned long)&xfer);
	if (ret < 0) {
		if (errno == ENXIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr,
		     void *buf, size_t len, uint32_t timeout_ms)
{
	(void)timeout_ms;

	struct i2c_msg_s msg = {
		.frequency = i2c->speed_hz,
		.addr      = addr,
		.flags     = I2C_M_READ,
		.buffer    = buf,
		.length    = len,
	};
	struct i2c_transfer_s xfer = {
		.msgv  = &msg,
		.msgc  = 1,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER,
			(unsigned long)&xfer);
	if (ret < 0) {
		if (errno == ENXIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr,
			   const void *tx, size_t tx_len,
			   void *rx, size_t rx_len,
			   uint32_t timeout_ms)
{
	(void)timeout_ms;

	struct i2c_msg_s msgs[2] = {
		{
			.frequency = i2c->speed_hz,
			.addr      = addr,
			.flags     = 0,
			.buffer    = (uint8_t *)tx,
			.length    = tx_len,
		},
		{
			.frequency = i2c->speed_hz,
			.addr      = addr,
			.flags     = I2C_M_READ,
			.buffer    = rx,
			.length    = rx_len,
		},
	};
	struct i2c_transfer_s xfer = {
		.msgv  = msgs,
		.msgc  = 2,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER,
			(unsigned long)&xfer);
	if (ret < 0) {
		if (errno == ENXIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
}

#endif /* CONFIG_OVE_I2C */
