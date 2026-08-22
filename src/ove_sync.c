/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/sync.h"

#if defined(CONFIG_OVE_SYNC) && defined(OVE_HEAP_SYNC)

#include "ove_backend_common.h"

int ove_mutex_create(ove_mutex_t *mtx)
{
	int rc = ove_check_param(mtx);
	if (rc)
		return rc;
	ove_mutex_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_mutex_init(mtx, storage);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx) {
		ove_mutex_deinit(mtx);
		OVE_BACKEND_FREE(mtx);
	}
}

int ove_recursive_mutex_create(ove_mutex_t *mtx)
{
	int rc = ove_check_param(mtx);
	if (rc)
		return rc;
	ove_mutex_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_recursive_mutex_init(mtx, storage);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_recursive_mutex_destroy(ove_mutex_t mtx)
{
	if (mtx) {
		ove_recursive_mutex_deinit(mtx);
		OVE_BACKEND_FREE(mtx);
	}
}

int ove_sem_create(ove_sem_t *sem, unsigned int initial, unsigned int max)
{
	int rc = ove_check_param(sem);
	if (rc)
		return rc;
	ove_sem_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_sem_init(sem, storage, initial, max);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_sem_destroy(ove_sem_t sem)
{
	if (sem) {
		ove_sem_deinit(sem);
		OVE_BACKEND_FREE(sem);
	}
}

int ove_event_create(ove_event_t *evt)
{
	int rc = ove_check_param(evt);
	if (rc)
		return rc;
	ove_event_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_event_init(evt, storage);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_event_destroy(ove_event_t evt)
{
	if (evt) {
		ove_event_deinit(evt);
		OVE_BACKEND_FREE(evt);
	}
}

int ove_condvar_create(ove_condvar_t *cv)
{
	int rc = ove_check_param(cv);
	if (rc)
		return rc;
	ove_condvar_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	rc = ove_condvar_init(cv, storage);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

void ove_condvar_destroy(ove_condvar_t cv)
{
	if (cv) {
		ove_condvar_deinit(cv);
		OVE_BACKEND_FREE(cv);
	}
}

#endif
