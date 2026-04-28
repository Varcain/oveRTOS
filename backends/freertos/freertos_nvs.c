/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/nvs.h"
#include "ove/fs.h"
#include "ove/log.h"
#include "ove_backend_common.h"
#include <string.h>
#include <stdio.h>

#define NVS_DIR "/nvs"
#define NVS_PATH_MAX 64

static int nvs_make_path(char *buf, size_t buf_len, const char *key)
{
	if (!ove_nvs_key_is_valid(key)) {
		return OVE_ERR_INVALID_PARAM;
	}
	int len = snprintf(buf, buf_len, "%s/%s", NVS_DIR, key);
	if (len < 0 || (size_t)len >= buf_len) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

int ove_nvs_init(void)
{
	/* NVS directory is created on SD card by the app if needed */
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
}

int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	int ret;

	ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}

	ret = ove_fs_open(&f, path, OVE_FS_O_READ);
	if (ret != OVE_OK) {
		return ret;
	}

	size_t file_size;
	ret = ove_fs_size(f, &file_size);
	if (ret != OVE_OK) {
		ove_fs_close(f);
		return ret;
	}

	size_t to_read = (file_size < buf_len) ? file_size : buf_len;
	size_t bytes_read;
	ret = ove_fs_read(f, buf, to_read, &bytes_read);
	ove_fs_close(f);

	if (ret != OVE_OK) {
		return ret;
	}

	if (out_len != NULL) {
		*out_len = bytes_read;
	}
	return OVE_OK;
}

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	int ret;

	ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}

	ret = ove_fs_open(&f, path, OVE_FS_O_WRITE | OVE_FS_O_CREATE);
	if (ret != OVE_OK) {
		return ret;
	}

	size_t bytes_written;
	ret = ove_fs_write(f, data, len, &bytes_written);
	ove_fs_close(f);

	if (ret != OVE_OK) {
		return ret;
	}

	return OVE_OK;
}

int ove_nvs_erase(const char *key)
{
	char path[NVS_PATH_MAX];
	int ret;

	ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}

	return ove_fs_unlink(path);
}
