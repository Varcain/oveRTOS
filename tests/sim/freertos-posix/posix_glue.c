/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "ove/irq.h"
#include "ove/time.h"

bool ove_is_in_isr(void)
{
	return false;
}

void vApplicationTickHook(void)
{
}

int ove_time_get_us(uint64_t *out)
{
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	TickType_t ticks = xTaskGetTickCount();
	*out = (uint64_t)ticks * (1000000 / configTICK_RATE_HZ);
	return OVE_OK;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	_Exit(1);
}
