/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/nvs.h"
#include "ove_backend_common.h"
#include <string.h>

#if defined(CONFIG_NVS)
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>

static struct nvs_fs nvs;
static int nvs_initialized;

/* Simple hash of key string to NVS id */
static uint16_t key_to_id(const char *key)
{
	uint16_t hash = 5381;
	while (*key) {
		hash = ((hash << 5) + hash) + (uint8_t)*key++;
	}
	return hash ? hash : 1;
}

int ove_nvs_init(void)
{
	const struct flash_area *fa;
	int ret;

	ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa);
	if (ret != 0) {
		return ove_errno_to_ove(-ret);
	}

	nvs.flash_device = flash_area_get_device(fa);
	nvs.offset = fa->fa_off;
	nvs.sector_size = 4096;
	nvs.sector_count = fa->fa_size / nvs.sector_size;
	flash_area_close(fa);

	ret = nvs_mount(&nvs);
	if (ret != 0) {
		return ove_errno_to_ove(-ret);
	}

	nvs_initialized = 1;
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
	nvs_initialized = 0;
}

int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	if (!nvs_initialized) {
		return OVE_ERR_NOT_REGISTERED;
	}

	ssize_t ret = nvs_read(&nvs, key_to_id(key), buf, buf_len);
	if (ret < 0) {
		return ove_errno_to_ove((int)-ret);
	}

	if (out_len != NULL) {
		*out_len = (size_t)ret;
	}
	return OVE_OK;
}

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	if (!nvs_initialized) {
		return OVE_ERR_NOT_REGISTERED;
	}

	ssize_t ret = nvs_write(&nvs, key_to_id(key), data, len);
	if (ret < 0) {
		return ove_errno_to_ove((int)-ret);
	}
	return OVE_OK;
}

int ove_nvs_erase(const char *key)
{
	if (!nvs_initialized) {
		return OVE_ERR_NOT_REGISTERED;
	}

	int ret = nvs_delete(&nvs, key_to_id(key));
	if (ret != 0) {
		return ove_errno_to_ove(-ret);
	}
	return OVE_OK;
}

#else /* !CONFIG_NVS */

int ove_nvs_init(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}
void ove_nvs_deinit(void)
{
}
int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	(void)key;
	(void)buf;
	(void)buf_len;
	(void)out_len;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_nvs_write(const char *key, const void *data, size_t len)
{
	(void)key;
	(void)data;
	(void)len;
	return OVE_ERR_NOT_SUPPORTED;
}
int ove_nvs_erase(const char *key)
{
	(void)key;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_NVS */
