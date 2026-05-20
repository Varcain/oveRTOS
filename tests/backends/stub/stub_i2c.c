/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Stub I2C HAL for the stub test backend — no real bus, no /dev/i2c-N.
 * Lets the public-API tests in tests/suites/test_i2c.c exercise
 * create/destroy/null-param paths on a host with no I2C hardware.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_I2C

#include "ove/hal/hal_i2c.h"

#include <string.h>

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	(void)cfg;
	i2c->fd = 1; /* non-negative sentinel marks "open" */
	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	i2c->fd = -1;
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)data;
	(void)len;
	(void)timeout_ns;
	return OVE_OK;
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)timeout_ns;
	if (buf != NULL && len > 0)
		memset(buf, 0, len);
	return OVE_OK;
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)tx;
	(void)tx_len;
	(void)timeout_ns;
	if (rx != NULL && rx_len > 0)
		memset(rx, 0, rx_len);
	return OVE_OK;
}

#ifdef CONFIG_OVE_ASYNC

#include <pthread.h>
#include "ove_backend_common.h"

static void *stub_i2c_async_worker(void *arg)
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

	rc = pthread_create(&tid, &attr, stub_i2c_async_worker, i2c);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return OVE_ERR_NO_MEMORY;
	return OVE_OK;
}

#endif /* CONFIG_OVE_ASYNC */

#endif /* CONFIG_OVE_I2C */
