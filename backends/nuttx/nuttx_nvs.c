/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/nvs.h"
#include "ove_backend_common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define NVS_DIR "/mnt/sd/nvs"
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
	mkdir(NVS_DIR, 0777);
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
}

int ove_nvs_read(const char *key, void *buf, size_t buf_len,
			  size_t *out_len)
{
	char path[NVS_PATH_MAX];
	int ret;

	ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return ove_errno_to_ove(errno);
	}

	ssize_t n = read(fd, buf, buf_len);
	int read_err = (n < 0) ? errno : 0;
	close(fd);

	if (n < 0) {
		return ove_errno_to_ove(read_err);
	}

	if (out_len != NULL) {
		*out_len = (size_t)n;
	}
	return OVE_OK;
}

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	char path[NVS_PATH_MAX];
	int ret;

	ret = nvs_make_path(path, sizeof(path), key);
	if (ret != OVE_OK) {
		return ret;
	}

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		return ove_errno_to_ove(errno);
	}

	ssize_t n = write(fd, data, len);
	int write_err = (n < 0) ? errno : 0;
	close(fd);

	if (n < 0) {
		return ove_errno_to_ove(write_err);
	}
	if ((size_t)n != len) {
		return OVE_ERR_NO_MEMORY;
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

	if (unlink(path) != 0) {
		return ove_errno_to_ove(errno);
	}
	return OVE_OK;
}
