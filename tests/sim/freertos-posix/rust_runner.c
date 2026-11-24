/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"
#include "ove/irq.h"

uint32_t ove_sync_context(void)
{
	/* Only test tasks call this query, so pre-scheduler and idle behavior is unmodeled. */
	return OVE_SYNC_CONTEXT_RESOURCE | OVE_SYNC_CONTEXT_BLOCKING | OVE_SYNC_CONTEXT_PRODUCER;
}

static int (*suite_fn)(void);

static void run_suite(void *arg)
{
	(void)arg;
	int rc = suite_fn();
	fflush(stdout);
	_Exit(rc == 0 ? 0 : 1);
}

int ove_freertos_posix_run(int (*suite)(void))
{
	static StaticTask_t task;
	static StackType_t stack[configMINIMAL_STACK_SIZE];
	suite_fn = suite;
	if (xTaskCreateStatic(run_suite, "tests", configMINIMAL_STACK_SIZE, NULL,
			      tskIDLE_PRIORITY + 1, stack, &task) == NULL)
		return 1;
	vTaskStartScheduler();
	return 1;
}
