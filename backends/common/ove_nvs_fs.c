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
#define NVS_PATH_MAX OVE_NVS_KEY_BUFFER_SIZE
#include <string.h>

size_t ove_nvs_key_max_len(void)
{
	const size_t prefix = sizeof(OVE_NVS_FS_DIR) - 1;
	return prefix + 2 < NVS_PATH_MAX ? NVS_PATH_MAX - prefix - 2 : 0;
}

static int nvs_make_path(char *buf, size_t buf_len, const char *key, size_t key_len)
{
	const size_t prefix = sizeof(OVE_NVS_FS_DIR) - 1;
	if (key_len > ove_nvs_key_max_len() || !ove_nvs_key_is_valid_n(key, key_len) ||
	    prefix >= buf_len || key_len >= buf_len - prefix - 1)
		return OVE_ERR_INVALID_PARAM;
	memcpy(buf, OVE_NVS_FS_DIR, prefix);
	buf[prefix] = '/';
	memcpy(buf + prefix + 1, key, key_len);
	buf[prefix + 1 + key_len] = 0;
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

int ove_nvs_read_n(const char *key, size_t key_len, void *buf, size_t buf_len, size_t *out_len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	size_t size = 0;

	if (buf == NULL && buf_len != 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = nvs_make_path(path, sizeof(path), key, key_len);
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

int ove_nvs_write_n(const char *key, size_t key_len, const void *data, size_t len)
{
	char path[NVS_PATH_MAX];
	ove_file_t f;
	const int flags = OVE_FS_O_WRITE | OVE_FS_O_CREATE | OVE_FS_O_TRUNC;

	if (data == NULL && len != 0) {
		return OVE_ERR_INVALID_PARAM;
	}
	int ret = nvs_make_path(path, sizeof(path), key, key_len);
	if (ret != OVE_OK) {
		return ret;
	}
	ret = ove_fs_open(&f, path, flags);
	if (ret == OVE_ERR_NOT_FOUND) {
		/* The reopen below reports whatever mkdir could not fix. */
		int mk = ove_fs_mkdir(OVE_NVS_FS_DIR);
		(void)mk;
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

int ove_nvs_erase_n(const char *key, size_t key_len)
{
	char path[NVS_PATH_MAX];

	int ret = nvs_make_path(path, sizeof(path), key, key_len);
	if (ret != OVE_OK) {
		return ret;
	}
	ret = ove_fs_unlink(path);
	/* Erasing a key that is not stored has no effect. */
	return ret == OVE_ERR_NOT_FOUND ? OVE_OK : ret;
}

int ove_nvs_read(const char *key, void *buf, size_t len, size_t *out_len)
{
	return key ? ove_nvs_read_n(key, strlen(key), buf, len, out_len) : OVE_ERR_INVALID_PARAM;
}
int ove_nvs_write(const char *key, const void *data, size_t len)
{
	return key ? ove_nvs_write_n(key, strlen(key), data, len) : OVE_ERR_INVALID_PARAM;
}
int ove_nvs_erase(const char *key)
{
	return key ? ove_nvs_erase_n(key, strlen(key)) : OVE_ERR_INVALID_PARAM;
}
