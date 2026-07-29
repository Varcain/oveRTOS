/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "FreeRTOS.h"
#include "semphr.h"
#include "ff.h"

/*
 * FatFs serializes each logical volume through these hooks. Use static
 * FreeRTOS mutex storage so CONFIG_OVE_ZERO_HEAP builds retain the same
 * filesystem contract and gain priority inheritance without hidden heap use.
 */
static StaticSemaphore_t sync_storage[_VOLUMES];
static SemaphoreHandle_t sync_handles[_VOLUMES];

int ff_cre_syncobj(BYTE volume, _SYNC_t *sync)
{
	if (sync == NULL || volume >= _VOLUMES || sync_handles[volume] != NULL) {
		return 0;
	}
	sync_handles[volume] = xSemaphoreCreateMutexStatic(&sync_storage[volume]);
	*sync = sync_handles[volume];
	return *sync != NULL;
}

int ff_del_syncobj(_SYNC_t sync)
{
	for (unsigned int volume = 0; volume < _VOLUMES; volume++) {
		if (sync_handles[volume] == sync) {
			vSemaphoreDelete(sync);
			sync_handles[volume] = NULL;
			return 1;
		}
	}
	return 0;
}

int ff_req_grant(_SYNC_t sync)
{
	return xSemaphoreTake(sync, _FS_TIMEOUT) == pdTRUE;
}

void ff_rel_grant(_SYNC_t sync)
{
	(void)xSemaphoreGive(sync);
}
