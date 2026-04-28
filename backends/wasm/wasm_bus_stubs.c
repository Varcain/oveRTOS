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
int ove_hal_uart_tx(ove_uart_t u, const void *d, size_t l, uint32_t t, size_t *w)
{
	(void)u;
	(void)d;
	(void)l;
	(void)t;
	(void)w;
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
int ove_hal_spi_transfer(ove_spi_t s, const void *tx, void *rx, size_t l, uint32_t t)
{
	(void)s;
	(void)tx;
	(void)rx;
	(void)l;
	(void)t;
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
int ove_hal_i2c_write(ove_i2c_t i, uint16_t a, const void *d, size_t l, uint32_t t)
{
	(void)i;
	(void)a;
	(void)d;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_read(ove_i2c_t i, uint16_t a, void *b, size_t l, uint32_t t)
{
	(void)i;
	(void)a;
	(void)b;
	(void)l;
	(void)t;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_hal_i2c_write_read(ove_i2c_t i, uint16_t a, const void *tx, size_t tl, void *rx, size_t rl,
			   uint32_t t)
{
	(void)i;
	(void)a;
	(void)tx;
	(void)tl;
	(void)rx;
	(void)rl;
	(void)t;
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
