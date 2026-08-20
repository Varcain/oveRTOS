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
#define FT5336_REG_GMODE 0xa4
#define FT5336_REG_CHIP_ID 0xa8
#define FT5336_CHIP_ID 0x51
#define FT5336_GMODE_POLLING 0x00
#define FT5336_MAX_TOUCHES 10
#define FT5336_INVALID_TOUCH_ID 0x0f

#ifndef CONFIG_OVE_FT5336_I2C_INSTANCE
#define CONFIG_OVE_FT5336_I2C_INSTANCE 0
#endif

#define I2C_TMO_NS 25000000ull /* 25 ms — touch i2c runs on the coordinator thread */

static ove_i2c_t g_ft_i2c;
static ove_i2c_storage_t g_ft_i2c_storage;
static int g_ft_initialized;
static int g_last_x;
static int g_last_y;

int ove_ft5336_init(void)
{
	uint8_t chip_id;
	uint8_t mode = FT5336_GMODE_POLLING;
	struct ove_i2c_cfg cfg = {
		.instance = CONFIG_OVE_FT5336_I2C_INSTANCE,
		.speed = OVE_I2C_SPEED_FAST,
	};
	if (g_ft_initialized)
		return OVE_OK;
	if (ove_i2c_init(&g_ft_i2c, &g_ft_i2c_storage, &cfg) != OVE_OK)
		return OVE_ERR_NOT_FOUND;
	/* An address-only probe is not supported consistently by every RTOS I2C
	 * backend. Reading and validating the documented ID proves both the bus
	 * transaction and that the expected controller is present. */
	if (ove_i2c_reg_read(g_ft_i2c, FT5336_ADDR, FT5336_REG_CHIP_ID, &chip_id,
			     sizeof(chip_id), I2C_TMO_NS) != OVE_OK ||
	    chip_id != FT5336_CHIP_ID) {
		ove_i2c_deinit(g_ft_i2c);
		g_ft_i2c = NULL;
		return OVE_ERR_NOT_FOUND;
	}
	/* Match ST's ft5336_TS_Start(): explicitly disable controller-generated
	 * interrupts and make fresh coordinates available to our periodic poller. */
	if (ove_i2c_reg_write(g_ft_i2c, FT5336_ADDR, FT5336_REG_GMODE, &mode, sizeof(mode),
			      I2C_TMO_NS) != OVE_OK) {
		ove_i2c_deinit(g_ft_i2c);
		g_ft_i2c = NULL;
		return OVE_ERR_BUS_ERROR;
	}
	g_ft_initialized = 1;
	return OVE_OK;
}

void ove_ft5336_deinit(void)
{
	if (!g_ft_initialized)
		return;
	ove_i2c_deinit(g_ft_i2c);
	g_ft_i2c = NULL;
	g_ft_initialized = 0;
}

int ove_ft5336_read(int *x, int *y, int *pressed)
{
	uint8_t status;
	uint8_t buf[4]; /* P1_XH, P1_XL, P1_YH, P1_YL */
	if (ove_i2c_reg_read(g_ft_i2c, FT5336_ADDR, FT5336_REG_TD_STATUS, &status,
			     sizeof(status), I2C_TMO_NS) != OVE_OK)
		return OVE_ERR_BUS_ERROR;

	int touches = status & 0x0f;
	int is_pressed = 0;
	/* Match ST's BSP and reject impossible touch counts (including the observed
	 * all-ones no-data frame) before interpreting the point registers. */
	if (touches > 0 && touches <= FT5336_MAX_TOUCHES &&
	    ove_i2c_reg_read(g_ft_i2c, FT5336_ADDR, FT5336_REG_P1_XH, buf, sizeof(buf),
			     I2C_TMO_NS) != OVE_OK)
		return OVE_ERR_BUS_ERROR;
	if (touches > 0 && touches <= FT5336_MAX_TOUCHES) {
		int raw_x = ((buf[0] & 0x0f) << 8) | buf[1];
		int raw_y = ((buf[2] & 0x0f) << 8) | buf[3];
		int touch_id = buf[2] >> 4;
		/* The controller is mounted in portrait orientation on the F746
		 * Discovery. Match ST's BSP_TS_Init(TS_SWAP_XY): LCD X comes from raw
		 * Y and LCD Y from raw X. Reject invalid IDs/coordinates rather than
		 * letting the evdev layer clamp a no-data frame to the bottom-right. */
		if (touch_id != FT5336_INVALID_TOUCH_ID && raw_x < 272 && raw_y < 480) {
			g_last_x = raw_y;
			g_last_y = raw_x;
			is_pressed = 1;
		}
	}
	if (pressed)
		*pressed = is_pressed;
	if (x)
		*x = g_last_x;
	if (y)
		*y = g_last_y;
	return OVE_OK;
}

#endif /* CONFIG_OVE_FT5336 */
