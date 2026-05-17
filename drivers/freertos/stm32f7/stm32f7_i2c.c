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
#include "stm32f7_init.h"
#include "stm32f7xx_hal.h"
#include <string.h>

static I2C_TypeDef *instance_to_periph(unsigned int instance)
{
	switch (instance) {
	case 0:
		return I2C1;
	case 1:
		return I2C2;
	case 2:
		return I2C3;
#ifdef I2C4
	case 3:
		return I2C4;
#endif
	default:
		return NULL;
	}
}

static uint32_t speed_to_timing(uint32_t speed_hz)
{
	/* STM32F7 I2C timing values (assumes 216 MHz system clock).
	 * These are typical values — production boards may need
	 * CubeMX-generated timing for exact specs. */
	if (speed_hz >= 1000000)
		return 0x00200404; /* Fast-mode Plus ~1 MHz */
	if (speed_hz >= 400000)
		return 0x00B01A4B; /* Fast-mode 400 kHz */
	return 0x10C0ECFF;	   /* Standard-mode 100 kHz */
}

int ove_hal_i2c_open(ove_i2c_t i2c, const struct ove_i2c_cfg *cfg)
{
	I2C_TypeDef *periph = instance_to_periph(cfg->instance);
	HAL_StatusTypeDef hal_ret;

	if (periph == NULL)
		return OVE_ERR_INVALID_PARAM;

	memset(&i2c->hal_handle, 0, sizeof(i2c->hal_handle));
	i2c->hal_handle.Instance = periph;
	i2c->hal_handle.Init.Timing = speed_to_timing(i2c->speed_hz);
	i2c->hal_handle.Init.OwnAddress1 = 0;
	i2c->hal_handle.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	i2c->hal_handle.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	i2c->hal_handle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	i2c->hal_handle.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;

	hal_ret = HAL_I2C_Init(&i2c->hal_handle);
	if (hal_ret != HAL_OK)
		return OVE_ERR_NOT_SUPPORTED;

	return OVE_OK;
}

void ove_hal_i2c_close(ove_i2c_t i2c)
{
	HAL_I2C_DeInit(&i2c->hal_handle);
}

int ove_hal_i2c_write(ove_i2c_t i2c, uint16_t addr, const void *data, size_t len,
		      uint64_t timeout_ns)
{
	HAL_StatusTypeDef ret;

	ret = HAL_I2C_Master_Transmit(&i2c->hal_handle, (uint16_t)(addr << 1), (uint8_t *)data,
				      (uint16_t)len, stm32f7_ns_to_hal_ms(timeout_ns));
	switch (ret) {
	case HAL_OK:
		return OVE_OK;
	case HAL_TIMEOUT:
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_BUS_NACK;
	}
}

int ove_hal_i2c_read(ove_i2c_t i2c, uint16_t addr, void *buf, size_t len, uint64_t timeout_ns)
{
	HAL_StatusTypeDef ret;

	ret = HAL_I2C_Master_Receive(&i2c->hal_handle, (uint16_t)(addr << 1), buf, (uint16_t)len,
				     stm32f7_ns_to_hal_ms(timeout_ns));
	switch (ret) {
	case HAL_OK:
		return OVE_OK;
	case HAL_TIMEOUT:
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_BUS_NACK;
	}
}

int ove_hal_i2c_write_read(ove_i2c_t i2c, uint16_t addr, const void *tx, size_t tx_len, void *rx,
			   size_t rx_len, uint64_t timeout_ns)
{
	HAL_StatusTypeDef ret;

	/* Use HAL_I2C_Mem_Read when tx_len == 1 or 2 (register access),
	 * otherwise fall back to two separate transfers. */
	if (tx_len <= 2) {
		uint16_t mem_addr;
		uint16_t mem_size;

		if (tx_len == 1) {
			mem_addr = ((const uint8_t *)tx)[0];
			mem_size = I2C_MEMADD_SIZE_8BIT;
		} else {
			mem_addr = (uint16_t)(((const uint8_t *)tx)[0] << 8 |
					      ((const uint8_t *)tx)[1]);
			mem_size = I2C_MEMADD_SIZE_16BIT;
		}

		ret = HAL_I2C_Mem_Read(&i2c->hal_handle, (uint16_t)(addr << 1), mem_addr, mem_size,
				       rx, (uint16_t)rx_len, stm32f7_ns_to_hal_ms(timeout_ns));
	} else {
		/* Multi-byte prefix: write then read with repeated start
		 * via sequential transfer API. */
		ret = HAL_I2C_Master_Sequential_Transmit_IT(&i2c->hal_handle, (uint16_t)(addr << 1),
							    (uint8_t *)tx, (uint16_t)tx_len,
							    I2C_FIRST_FRAME);
		if (ret != HAL_OK)
			goto err;

		ret = HAL_I2C_Master_Sequential_Receive_IT(&i2c->hal_handle, (uint16_t)(addr << 1),
							   rx, (uint16_t)rx_len, I2C_LAST_FRAME);
	}

err:
	switch (ret) {
	case HAL_OK:
		return OVE_OK;
	case HAL_TIMEOUT:
		return OVE_ERR_TIMEOUT;
	default:
		return OVE_ERR_BUS_NACK;
	}
}

#endif /* CONFIG_OVE_I2C */
