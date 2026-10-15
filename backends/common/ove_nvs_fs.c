/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NVS for backends with a filesystem (FreeRTOS, NuttX): each key is a file in
 * OVE_NVS_FS_DIR on the volume the application mounted with ove_fs_mount().
 * The path is volume-relative like every ove_fs path, so on NuttX's default
 * SD mount it is /mnt/sd/nvs.
 */

#include "ove/nvs.h"
#include "ove/fs.h"
#include "ove_backend_common.h"
#include <stdio.h>

#ifndef OVE_NVS_FS_DIR
#define OVE_NVS_FS_DIR "/nvs"
#endif
#define NVS_PATH_MAX 96

static int nvs_make_path(char *buf, size_t buf_len, const char *key)
{
	if (!ove_nvs_key_is_valid(key)) {
		return OVE_ERR_INVALID_PARAM;
	}
	int len = snprintf(buf, buf_len, "%s/%s", OVE_NVS_FS_DIR, key);
	if (len < 0 || (size_t)len >= buf_len) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_nvs_init(void)
{
	/* The volume may not be mounted yet; the first write creates the
	 * directory. */
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
}

int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	size_t size = 0;

	if (buf == NULL && buf_len != 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}
	ret = ove_fs_open(&f, path, OVE_FS_O_READ);
	if (ret != OVE_OK) {
		return ret;
	}

	ret = ove_fs_size(f, &size);
	size_t to_read = size < buf_len ? size : buf_len;
	if (ret == OVE_OK && to_read != 0) {
		size_t bytes_read = 0;
		ret = ove_fs_read(f, buf, to_read, &bytes_read);
		if (ret == OVE_OK && bytes_read != to_read) {
			ret = OVE_ERR_IO;
		}
	}
	(void)ove_fs_close(f);

	/* The stored length, not the copied one: callers detect truncation
	 * and size-query with (NULL, 0). */
	if (ret == OVE_OK && out_len != NULL) {
		*out_len = size;
	}
	return ret;
}

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	const int flags = OVE_FS_O_WRITE | OVE_FS_O_CREATE | OVE_FS_O_TRUNC;

	if (data == NULL && len != 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}
	ret = ove_fs_open(&f, path, flags);
	if (ret == OVE_ERR_NOT_FOUND) {
		(void)ove_fs_mkdir(OVE_NVS_FS_DIR);
		ret = ove_fs_open(&f, path, flags);
	}
	if (ret != OVE_OK) {
		return ret;
	}

	size_t bytes_written = len;
	if (len != 0) {
		ret = ove_fs_write(f, data, len, &bytes_written);
	}
	/* Closing commits the data, so its result counts too. */
	int close_ret = ove_fs_close(f);
	if (ret != OVE_OK) {
		return ret;
	}
	/* A short write (medium full) must not pass as success. */
	if (bytes_written != len) {
		return OVE_ERR_NO_MEMORY;
	}
	return close_ret;
}

int ove_nvs_erase(const char *key)
{
	char path[NVS_PATH_MAX];

	int ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}
	ret = ove_fs_unlink(path);
	/* Erasing a key that is not stored has no effect. */
	return ret == OVE_ERR_NOT_FOUND ? OVE_OK : ret;
}
