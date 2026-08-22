/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/eventgroup.h"

#if defined(CONFIG_OVE_EVENTGROUP) && defined(OVE_HEAP_EVENTGROUP)

#include "ove_backend_common.h"

int ove_eventgroup_create(ove_eventgroup_t *eg)
{
	int rc = ove_check_param(eg);
	if (rc)
		return rc;
	ove_eventgroup_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_eventgroup_init(eg, storage);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_eventgroup_destroy(ove_eventgroup_t eg)
{
	if (eg) {
		ove_eventgroup_deinit(eg);
		OVE_BACKEND_FREE(eg);
	}
}

#endif
