/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ove/ove.h"
#include <assert.h>
#include <string.h>

static char opened[OVE_NVS_KEY_BUFFER_SIZE];
static unsigned calls;
int ove_fs_open(ove_file_t *file, const char *path, int flags)
{
	(void)file;
	(void)flags;
	++calls;
	assert(strlen(path) < sizeof(opened));
	strcpy(opened, path);
	return OVE_ERR_NOT_FOUND;
}
int ove_fs_size(ove_file_t file, size_t *size)
{
	(void)file;
	(void)size;
	return OVE_ERR_IO;
}
int ove_fs_read(ove_file_t file, void *data, size_t len, size_t *read)
{
	(void)file;
	(void)data;
	(void)len;
	(void)read;
	return OVE_ERR_IO;
}
int ove_fs_write(ove_file_t file, const void *data, size_t len, size_t *written)
{
	(void)file;
	(void)data;
	(void)len;
	(void)written;
	return OVE_ERR_IO;
}
int ove_fs_close(ove_file_t file)
{
	(void)file;
	return OVE_OK;
}
int ove_fs_mkdir(const char *path)
{
	(void)path;
	return OVE_OK;
}
int ove_fs_unlink(const char *path)
{
	(void)path;
	return OVE_ERR_NOT_FOUND;
}
int main(void)
{
	const size_t prefix_len = sizeof(OVE_TEST_NVS_PREFIX) - 1;
	const size_t max = prefix_len + 2 < OVE_NVS_KEY_BUFFER_SIZE
				   ? OVE_NVS_KEY_BUFFER_SIZE - prefix_len - 2
				   : 0;
	assert(ove_nvs_key_max_len() == max);
	char key[OVE_NVS_KEY_BUFFER_SIZE];
	memset(key, 'k', sizeof(key));
	size_t len = 0;
	if (max) {
		assert(ove_nvs_read_n(key, max, NULL, 0, &len) == OVE_ERR_NOT_FOUND);
		assert(calls == 1 && strlen(opened) == OVE_NVS_KEY_BUFFER_SIZE - 1);
		assert(memcmp(opened, OVE_TEST_NVS_PREFIX "/", prefix_len + 1) == 0);
		assert(memcmp(opened + prefix_len + 1, key, max) == 0);
	}
	unsigned before = calls;
	assert(ove_nvs_read_n(key, max + 1, NULL, 0, &len) == OVE_ERR_INVALID_PARAM);
	key[0] = '.';
	assert(ove_nvs_read_n(key, 1, NULL, 0, &len) == OVE_ERR_INVALID_PARAM);
	key[0] = 0;
	assert(ove_nvs_read_n(key, 1, NULL, 0, &len) == OVE_ERR_INVALID_PARAM);
	assert(calls == before);
	return 0;
}
