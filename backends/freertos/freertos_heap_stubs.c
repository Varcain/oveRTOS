/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "FreeRTOS.h"
#include "task.h"

void *pvPortMalloc(size_t xWantedSize)
{
	(void)xWantedSize;
	configASSERT(0); /* Must never be called in zero-heap mode */
	return NULL;
}

void vPortFree(void *pv)
{
	(void)pv;
	/* vPortFree(NULL) is called by FreeRTOS for statically-created
	 * tasks on deletion — this is a no-op, not an error. */
	if (pv != NULL) {
		configASSERT(0);
	}
}

size_t xPortGetFreeHeapSize(void)
{
	return 0;
}
