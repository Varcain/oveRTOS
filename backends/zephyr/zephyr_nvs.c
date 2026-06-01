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

/*
 * Zephyr NVS stores blobs keyed by uint16 id, so string keys are hashed.
 * A bare hash silently corrupts data on collision (two distinct keys → one
 * id → each overwrites/reads the other). We defend against that: the value
 * lives under a 15-bit hash id, and the key string itself is stored under
 * the companion id (top bit set). Reads/writes verify the stored key equals
 * the requested key, so a colliding key reads back NOT_FOUND and a write
 * that would clobber a different key's id is rejected instead of silently
 * corrupting it.
 */
#define NVS_KEY_STORE_MAX 64u

static uint16_t value_id(const char *key)
{
	uint16_t hash = 5381;
	while (*key) {
		hash = ((hash << 5) + hash) + (uint8_t)*key++;
	}
	hash &= 0x7FFFu; /* reserve the top bit for the companion key id */
	return hash ? hash : 1u;
}

/* Copy `key` into `out` NUL-terminated and truncated to NVS_KEY_STORE_MAX,
 * so the stored form and the compared form are always identical. Returns
 * the number of bytes to store (including the NUL). */
static size_t key_store_form(const char *key, char out[NVS_KEY_STORE_MAX])
{
	size_t i = 0;
	for (; key[i] != '\0' && i < NVS_KEY_STORE_MAX - 1u; i++) {
		out[i] = key[i];
	}
	out[i] = '\0';
	return i + 1u;
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

	uint16_t vid = value_id(key);
	char want[NVS_KEY_STORE_MAX];
	char stored[NVS_KEY_STORE_MAX];
	(void)key_store_form(key, want);

	ssize_t kn = nvs_read(&nvs, vid | 0x8000u, stored, sizeof(stored));
	if (kn <= 0) {
		return OVE_ERR_NOT_FOUND;
	}
	stored[((size_t)kn < sizeof(stored)) ? (size_t)kn : sizeof(stored) - 1u] = '\0';
	if (strcmp(stored, want) != 0) {
		/* Different key hashed to this id — the requested key is absent. */
		return OVE_ERR_NOT_FOUND;
	}

	ssize_t ret = nvs_read(&nvs, vid, buf, buf_len);
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

	uint16_t vid = value_id(key);
	uint16_t kid = vid | 0x8000u;
	char want[NVS_KEY_STORE_MAX];
	char stored[NVS_KEY_STORE_MAX];
	size_t klen = key_store_form(key, want);

	ssize_t kn = nvs_read(&nvs, kid, stored, sizeof(stored));
	if (kn > 0) {
		stored[((size_t)kn < sizeof(stored)) ? (size_t)kn : sizeof(stored) - 1u] = '\0';
		if (strcmp(stored, want) != 0) {
			/* Hash collision with a different key — refuse rather than
			 * clobber the other key's value. */
			return OVE_ERR_NO_MEMORY;
		}
	}

	ssize_t kr = nvs_write(&nvs, kid, want, klen);
	if (kr < 0) {
		return ove_errno_to_ove((int)-kr);
	}
	ssize_t ret = nvs_write(&nvs, vid, data, len);
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

	uint16_t vid = value_id(key);
	uint16_t kid = vid | 0x8000u;
	char want[NVS_KEY_STORE_MAX];
	char stored[NVS_KEY_STORE_MAX];
	(void)key_store_form(key, want);

	ssize_t kn = nvs_read(&nvs, kid, stored, sizeof(stored));
	if (kn > 0) {
		stored[((size_t)kn < sizeof(stored)) ? (size_t)kn : sizeof(stored) - 1u] = '\0';
		if (strcmp(stored, want) != 0) {
			/* A different key owns this id; nothing of ours to erase. */
			return OVE_OK;
		}
	}
	(void)nvs_delete(&nvs, vid);
	(void)nvs_delete(&nvs, kid);
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
