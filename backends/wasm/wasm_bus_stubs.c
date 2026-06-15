/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * WASM bus driver stubs.
 *
 * Provides all HAL functions for UART, SPI, I2C, I2S that return
 * OVE_ERR_NOT_SUPPORTED.  Prevents link errors when apps enable
 * these modules in the WASM build.
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_UART
#include "ove/hal/hal_uart.h"

int ove_hal_uart_open(ove_uart_t u, const struct ove_uart_cfg *c)
{
	(void)u;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_uart_close(ove_uart_t u)
{
	(void)u;
}
int ove_hal_uart_tx(ove_uart_t uart, const void *data, size_t len, uint64_t timeout_ns,
		    size_t *bytes_written)
{
	(void)uart;
	(void)data;
	(void)len;
	(void)timeout_ns;
	(void)bytes_written;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_uart_rx_enable(ove_uart_t u)
{
	(void)u;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_uart_tx_flush(ove_uart_t u)
{
	(void)u;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif

#ifdef CONFIG_OVE_SPI
#include "ove/hal/hal_spi.h"

int ove_hal_spi_open(ove_spi_t s, const struct ove_spi_cfg *c)
{
	(void)s;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_spi_close(ove_spi_t s)
{
	(void)s;
}
int ove_hal_spi_transfer(ove_spi_t spi, const void *tx, void *rx, size_t len, uint64_t timeout_ns)
{
	(void)spi;
	(void)tx;
	(void)rx;
	(void)len;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_spi_transfer_async(ove_spi_t spi, const void *tx, void *rx, size_t len)
{
	(void)spi;
	(void)tx;
	(void)rx;
	(void)len;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif

#ifdef CONFIG_OVE_I2C
#include "ove/hal/hal_i2c.h"

int ove_hal_i2c_open(ove_i2c_t i, const struct ove_i2c_cfg *c)
{
	(void)i;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_i2c_close(ove_i2c_t i)
{
	(void)i;
}
int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)data;
	(void)len;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)buf;
	(void)len;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	(void)i2c;
	(void)addr;
	(void)tx;
	(void)tx_len;
	(void)rx;
	(void)rx_len;
	(void)timeout_ns;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_write_read_async(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len,
				 void *rx, size_t rx_len)
{
	(void)i2c;
	(void)addr;
	(void)tx;
	(void)tx_len;
	(void)rx;
	(void)rx_len;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif

#ifdef CONFIG_OVE_I2S
#include "ove/hal/hal_i2s.h"

int ove_hal_i2s_open(ove_i2s_t i, const struct ove_i2s_cfg *c)
{
	(void)i;
	(void)c;
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_hal_i2s_close(ove_i2s_t i)
{
	(void)i;
}
int ove_hal_i2s_start(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_stop(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_pause(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2s_resume(ove_i2s_t i)
{
	(void)i;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif
