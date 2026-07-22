/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * FT5336 capacitive-touch driver (STM32F746-Discovery), a pure ove_i2c client at
 * address 0x38. Read the touch-count + point-1 X/Y registers; the personality's
 * evdev class polls ove_ft5336_read from the run-loop tick.
 */

#include "ove_config.h"

#if defined(CONFIG_OVE_FT5336)

#include "ove/ft5336.h"
#include "ove/i2c.h"
#include "ove/types.h"

#define FT5336_ADDR 0x38
#define FT5336_REG_TD_STATUS 0x02 /* low nibble = number of active touches */
#define FT5336_REG_P1_XH 0x03	  /* [3:0] = X[11:8]; [7:6] = event flag */
#define FT5336_REG_P1_XL 0x04
#define FT5336_REG_P1_YH 0x05 /* [3:0] = Y[11:8] */
#define FT5336_REG_P1_YL 0x06
#define FT5336_REG_CHIP_ID 0xa8
#define FT5336_CHIP_ID 0x51

#ifndef CONFIG_OVE_FT5336_I2C_INSTANCE
#define CONFIG_OVE_FT5336_I2C_INSTANCE 0
#endif

#define I2C_TMO_NS 25000000ull /* 25 ms — touch i2c runs on the coordinator thread */

static ove_i2c_t g_ft_i2c;

int ove_ft5336_init(void)
{
	uint8_t chip_id;
	struct ove_i2c_cfg cfg = {
		.instance = CONFIG_OVE_FT5336_I2C_INSTANCE,
		.speed = OVE_I2C_SPEED_FAST,
	};
	if (ove_i2c_create(&g_ft_i2c, &cfg) != OVE_OK)
		return OVE_ERR_NOT_FOUND;
	/* An address-only probe is not supported consistently by every RTOS I2C
	 * backend. Reading and validating the documented ID proves both the bus
	 * transaction and that the expected controller is present. */
	if (ove_i2c_reg_read(g_ft_i2c, FT5336_ADDR, FT5336_REG_CHIP_ID, &chip_id,
			     sizeof(chip_id), I2C_TMO_NS) != OVE_OK ||
	    chip_id != FT5336_CHIP_ID)
		return OVE_ERR_NOT_FOUND;
	return OVE_OK;
}

int ove_ft5336_read(int *x, int *y, int *pressed)
{
	uint8_t buf[5]; /* TD_STATUS, P1_XH, P1_XL, P1_YH, P1_YL */
	if (ove_i2c_reg_read(g_ft_i2c, FT5336_ADDR, FT5336_REG_TD_STATUS, buf, sizeof(buf),
			     I2C_TMO_NS) != OVE_OK)
		return OVE_ERR_BUS_ERROR;
	int touches = buf[0] & 0x0f;
	if (pressed)
		*pressed = touches > 0;
	int raw_x = ((buf[1] & 0x0f) << 8) | buf[2];
	int raw_y = ((buf[3] & 0x0f) << 8) | buf[4];
	/* The controller is mounted in portrait orientation on the F746 Discovery.
	 * Match ST's BSP_TS_Init(TS_SWAP_XY): LCD X comes from raw Y and LCD Y from
	 * raw X, yielding the panel's native 480x272 landscape coordinates. */
	if (x)
		*x = raw_y;
	if (y)
		*y = raw_x;
	return OVE_OK;
}

#endif /* CONFIG_OVE_FT5336 */
