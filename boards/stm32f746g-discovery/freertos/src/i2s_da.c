/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "i2s_da.h"
#include <stddef.h>

/* Internal helper macro to safely call driver function */
#define I2S_CALL_OP(op, ...) \
	((driver != NULL && driver->op != NULL) ? driver->op(__VA_ARGS__) : 0)

/* Internal helper macro for void functions */
#define I2S_CALL_OP_VOID(op, ...) \
	do { \
		if (driver != NULL && driver->op != NULL) { \
			driver->op(__VA_ARGS__); \
		} \
	} while (0)

static const struct i2s_drv_ops *driver = NULL;

int i2s_set_driver(struct i2s_drv_ops *ops)
{
	if (ops == NULL) {
		return 0;
	}

	/* Validate that required operations are provided */
	if (ops->init == NULL || ops->getRxBuffer == NULL || 
	    ops->getTxBuffer == NULL || ops->startStream == NULL) {
		return 0;
	}

	driver = ops;
	return 1;
}

int i2s_is_initialized(void)
{
	return (driver != NULL) ? 1 : 0;
}

void i2s_init(void)
{
	I2S_CALL_OP_VOID(init);
}

unsigned long i2s_getRxBuffer(void)
{
	if (driver != NULL && driver->getRxBuffer != NULL) {
		return driver->getRxBuffer();
	}
	return 0UL;
}

unsigned long i2s_getTxBuffer(void)
{
	if (driver != NULL && driver->getTxBuffer != NULL) {
		return driver->getTxBuffer();
	}
	return 0UL;
}

int i2s_rxBufferRdy(void)
{
	if (driver != NULL && driver->rxBufferRdy != NULL) {
		return driver->rxBufferRdy();
	}
	return -1;
}

unsigned int i2s_xferCnt(void)
{
	if (driver != NULL && driver->xferCnt != NULL) {
		return driver->xferCnt();
	}
	return 0U;
}

void i2s_setRxCompleteCb(i2s_driver_rxCompleteCb cb)
{
	I2S_CALL_OP_VOID(setRxCompleteCb, cb);
}

void i2s_startStream(void)
{
	I2S_CALL_OP_VOID(startStream);
}

void i2s_pauseStream(void)
{
	I2S_CALL_OP_VOID(pauseStream);
}

void i2s_resumeStream(void)
{
	I2S_CALL_OP_VOID(resumeStream);
}
