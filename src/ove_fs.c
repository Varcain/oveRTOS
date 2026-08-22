/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/fs.h"

#if defined(CONFIG_OVE_FS) && defined(OVE_HEAP_FS)

#include "ove_backend_common.h"

int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	if (!file || !path)
		return OVE_ERR_INVALID_PARAM;
	ove_file_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	int rc = ove_fs_open_init(file, storage, path, flags);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

int ove_fs_close(ove_file_t file)
{
	int rc = ove_fs_close_deinit(file);
	if (rc == OVE_OK)
		OVE_BACKEND_FREE(file);
	return rc;
}

int ove_fs_opendir(ove_dir_t *dir, const char *path)
{
	if (!dir || !path)
		return OVE_ERR_INVALID_PARAM;
	ove_dir_storage_t *storage = OVE_BACKEND_MALLOC(sizeof(*storage));
	if (!storage)
		return OVE_ERR_NO_MEMORY;
	int rc = ove_fs_opendir_init(dir, storage, path);
	if (rc != OVE_OK)
		OVE_BACKEND_FREE(storage);
	return rc;
}

int ove_fs_closedir(ove_dir_t dir)
{
	int rc = ove_fs_closedir_deinit(dir);
	if (rc == OVE_OK)
		OVE_BACKEND_FREE(dir);
	return rc;
}

#endif
