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

/* Linux I2C userspace interface */
#ifdef __linux__
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#endif

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
#ifdef __linux__
	char path[32];

	snprintf(path, sizeof(path), "/dev/i2c-%u", cfg->instance);

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		/* No i2c-dev present (CI / sim): no-op stub mode. */
		i2c->fd = -1;
		return OVE_OK;
	}

	i2c->fd = fd;
	return OVE_OK;
#else
	/* Non-Linux POSIX: simulation stub */
	(void)cfg;
	i2c->fd = -1;
	return OVE_OK;
#endif
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	if (i2c->fd >= 0) {
		close(i2c->fd);
		i2c->fd = -1;
	}
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	(void)timeout_ns;

#ifdef __linux__
	if (i2c->fd < 0) {
		(void)addr;
		(void)data;
		(void)len;
		return OVE_OK;
	}
	struct i2c_msg msg = {
		.addr = addr,
		.flags = 0,
		.len = (uint16_t)len,
		.buf = (uint8_t *)data,
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};

	if (ioctl(i2c->fd, I2C_RDWR, &rdwr) < 0) {
		if (errno == ENXIO || errno == EREMOTEIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
#else
	(void)i2c;
	(void)addr;
	(void)data;
	(void)len;
	return OVE_OK;
#endif
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	(void)timeout_ns;

#ifdef __linux__
	if (i2c->fd < 0) {
		(void)addr;
		if (buf != NULL)
			memset(buf, 0, len);
		return OVE_OK;
	}
	struct i2c_msg msg = {
		.addr = addr,
		.flags = I2C_M_RD,
		.len = (uint16_t)len,
		.buf = buf,
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = &msg,
		.nmsgs = 1,
	};

	if (ioctl(i2c->fd, I2C_RDWR, &rdwr) < 0) {
		if (errno == ENXIO || errno == EREMOTEIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
#else
	(void)i2c;
	(void)addr;
	if (buf != NULL)
		memset(buf, 0, len);
	return OVE_OK;
#endif
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	(void)timeout_ns;

#ifdef __linux__
	if (i2c->fd < 0) {
		(void)addr;
		(void)tx;
		(void)tx_len;
		if (rx != NULL)
			memset(rx, 0, rx_len);
		return OVE_OK;
	}
	struct i2c_msg msgs[2] = {
		{
			.addr = addr,
			.flags = 0,
			.len = (uint16_t)tx_len,
			.buf = (uint8_t *)tx,
		},
		{
			.addr = addr,
			.flags = I2C_M_RD,
			.len = (uint16_t)rx_len,
			.buf = rx,
		},
	};
	struct i2c_rdwr_ioctl_data rdwr = {
		.msgs = msgs,
		.nmsgs = 2,
	};

	if (ioctl(i2c->fd, I2C_RDWR, &rdwr) < 0) {
		if (errno == ENXIO || errno == EREMOTEIO)
			return OVE_ERR_BUS_NACK;
		return OVE_ERR_BUS_ERROR;
	}
	return OVE_OK;
#else
	(void)i2c;
	(void)addr;
	(void)tx;
	(void)tx_len;
	if (rx != NULL)
		memset(rx, 0, rx_len);
	return OVE_OK;
#endif
}

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>

static void *i2c_async_worker(void *arg)
{
	ove_i2c_t i2c = (ove_i2c_t)arg;
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
	return NULL;
}

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	pthread_t tid;
	pthread_attr_t attr;
	int rc;

	i2c->pending_addr = addr;
	i2c->pending_tx = tx;
	i2c->pending_tx_len = tx_len;
	i2c->pending_rx = rx;
	i2c->pending_rx_len = rx_len;

	if (pthread_attr_init(&attr) != 0)
		return OVE_ERR_NO_MEMORY;
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&tid, &attr, i2c_async_worker, i2c);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
