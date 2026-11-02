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
#include <limits.h>

#include <nuttx/i2c/i2c_master.h>

/* Translate the errno of a failed I2CIOC_TRANSFER.  The STM32F7 driver
 * reports a NACK as EADDRNOTAVAIL (address) or ECOMM (data), never ENXIO;
 * ENXIO is what other NuttX I2C drivers use.  Arbitration loss (EAGAIN)
 * and a bus still busy after the transfer (EBUSY) are bus faults, not a
 * transfer in flight. */
static int i2c_errno_to_ove(int e)
{
	switch (e) {
	case ENXIO:
	case EADDRNOTAVAIL:
	case ECOMM:
		return OVE_ERR_BUS_NACK;
	case ETIMEDOUT:
	case ETIME:
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_BUS_ERROR;
	}
}

int ove_hal_i2c_validate_transfer(ove_i2c_t i2c, size_t tx_len, size_t rx_len, int asynchronous)
{
	(void)i2c;
	(void)asynchronous;
	if (tx_len > (size_t)INT_MAX || rx_len > (size_t)INT_MAX || tx_len > (size_t)SSIZE_MAX ||
	    rx_len > (size_t)SSIZE_MAX)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	char path[32];

	(void)snprintf(path, sizeof(path), "/dev/i2c%u", cfg->instance);

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

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, len, 0, 0);
	if (validation != OVE_OK)
		return validation;

	(void)timeout_ns;

	struct i2c_msg_s msg = {
		.frequency = i2c->speed_hz,
		.addr = addr,
		.flags = 0,
		.buffer = (uint8_t *)data,
		.length = (ssize_t)len,
	};
	struct i2c_transfer_s xfer = {
		.msgv = &msg,
		.msgc = 1,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
	if (ret < 0)
		return i2c_errno_to_ove(errno);
	return OVE_OK;
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, 0, len, 0);
	if (validation != OVE_OK)
		return validation;

	(void)timeout_ns;

	struct i2c_msg_s msg = {
		.frequency = i2c->speed_hz,
		.addr = addr,
		.flags = I2C_M_READ,
		.buffer = buf,
		.length = (ssize_t)len,
	};
	struct i2c_transfer_s xfer = {
		.msgv = &msg,
		.msgc = 1,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
	if (ret < 0)
		return i2c_errno_to_ove(errno);
	return OVE_OK;
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

	struct i2c_msg_s msgs[2] = {
		{
			.frequency = i2c->speed_hz,
			.addr = addr,
			.flags = I2C_M_NOSTOP,
			.buffer = (uint8_t *)tx,
			.length = (ssize_t)tx_len,
		},
		{
			.frequency = i2c->speed_hz,
			.addr = addr,
			.flags = I2C_M_READ,
			.buffer = rx,
			.length = (ssize_t)rx_len,
		},
	};
	struct i2c_transfer_s xfer = {
		.msgv = msgs,
		.msgc = 2,
	};

	int ret = ioctl(i2c->fd, I2CIOC_TRANSFER, (unsigned long)&xfer);
	if (ret < 0)
		return i2c_errno_to_ove(errno);
	return OVE_OK;
}

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>

int ove_hal_i2c_async_prepare(void)
{
#if defined(CONFIG_OVE_ZERO_HEAP) && defined(CONFIG_DEBUG_ASSERTIONS)
	/* Native worker allocation is unavailable under the debug heap lock. */
	return OVE_ERR_NOT_SUPPORTED;
#else
	return OVE_OK;
#endif
}

void ove_hal_i2c_async_retire(ove_i2c_t i2c)
{
	/* Neither the worker nor submitter accesses the controller after handoff. */
	ove_i2c_async_retired(i2c);
}

static void i2c_clear_transfer(ove_i2c_t i2c)
{
	i2c->pending_addr = 0;
	i2c->pending_tx = NULL;
	i2c->pending_tx_len = 0;
	i2c->pending_rx = NULL;
	i2c->pending_rx_len = 0;
}

static void *nuttx_i2c_async_worker(void *arg)
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

	i2c_clear_transfer(i2c);
	ove_i2c_async_complete(i2c, result);
	return NULL;
}

int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	if (addr > 0x7f)
		return OVE_ERR_INVALID_PARAM;
	int validation = ove_hal_i2c_validate_transfer(i2c, tx_len, rx_len, 1);
	if (validation != OVE_OK)
		return validation;

	pthread_t tid;
	pthread_attr_t attr;
	int rc;

	i2c->pending_addr = addr;
	i2c->pending_tx = tx;
	i2c->pending_tx_len = tx_len;
	i2c->pending_rx = rx;
	i2c->pending_rx_len = rx_len;

	if (pthread_attr_init(&attr) != 0) {
		i2c_clear_transfer(i2c);
		return OVE_ERR_NO_MEMORY;
	}
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	rc = pthread_create(&tid, &attr, nuttx_i2c_async_worker, i2c);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		i2c_clear_transfer(i2c);
		return OVE_ERR_NO_MEMORY;
	}
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
