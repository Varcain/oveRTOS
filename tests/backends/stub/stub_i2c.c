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
	i2c->fd = 1;  /* non-negative sentinel marks "open" */
	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	i2c->fd = -1;
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr,
		      const void *data, size_t len, uint32_t timeout_ms)
{
	(void)i2c; (void)addr; (void)data; (void)len; (void)timeout_ms;
	return OVE_OK;
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr,
		     void *buf, size_t len, uint32_t timeout_ms)
{
	(void)i2c; (void)addr; (void)timeout_ms;
	if (buf != NULL && len > 0)
		memset(buf, 0, len);
	return OVE_OK;
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr,
			   const void *tx, size_t tx_len,
			   void *rx, size_t rx_len, uint32_t timeout_ms)
{
	(void)i2c; (void)addr; (void)tx; (void)tx_len; (void)timeout_ms;
	if (rx != NULL && rx_len > 0)
		memset(rx, 0, rx_len);
	return OVE_OK;
}

#endif /* CONFIG_OVE_I2C */
