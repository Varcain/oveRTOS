/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/workqueue.h"

#if defined(CONFIG_OVE_WORKQUEUE) && defined(OVE_HEAP_WORKQUEUE)

#include "ove_backend_common.h"

int ove_workqueue_create(ove_workqueue_t *wq, const char *name, ove_prio_t priority,
			 size_t stack_size)
{
	int rc = ove_check_param(wq);
	if (rc)
		return rc;

	ove_workqueue_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_workqueue_init(wq, storage, name, priority, stack_size, NULL);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_workqueue_destroy(ove_workqueue_t wq)
{
	if (wq) {
		ove_workqueue_deinit(wq);
		OVE_BACKEND_FREE(wq);
	}
}

#endif
