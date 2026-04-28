/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/ove.h"
#include "ove_backend_common.h"
#include <string.h>

#define NVS_MAX_ENTRIES 64
#define NVS_MAX_KEY_LEN 64
#define NVS_MAX_VAL_LEN 256

struct nvs_entry {
	char key[NVS_MAX_KEY_LEN];
	uint8_t value[NVS_MAX_VAL_LEN];
	size_t len;
	int used;
};

static struct nvs_entry nvs_store[NVS_MAX_ENTRIES];

static struct nvs_entry *nvs_find(const char *key)
{
	for (int i = 0; i < NVS_MAX_ENTRIES; i++) {
		if (nvs_store[i].used && strcmp(nvs_store[i].key, key) == 0) {
			return &nvs_store[i];
		}
	}
	return NULL;
}

int ove_nvs_init(void)
{
	memset(nvs_store, 0, sizeof(nvs_store));
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
	memset(nvs_store, 0, sizeof(nvs_store));
}

int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	if (!key || !buf) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
	if (!e) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	size_t copy = e->len < buf_len ? e->len : buf_len;
	memcpy(buf, e->value, copy);
	if (out_len) {
		*out_len = e->len;
	}
	return OVE_OK;
}

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	if (!key || !data || len > NVS_MAX_VAL_LEN) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
	if (!e) {
		/* Find free slot */
		for (int i = 0; i < NVS_MAX_ENTRIES; i++) {
			if (!nvs_store[i].used) {
				e = &nvs_store[i];
				break;
			}
		}
		if (!e) {
			return OVE_ERR_NO_MEMORY;
		}
		strncpy(e->key, key, NVS_MAX_KEY_LEN - 1);
		e->key[NVS_MAX_KEY_LEN - 1] = '\0';
		e->used = 1;
	}
	memcpy(e->value, data, len);
	e->len = len;
	return OVE_OK;
}

int ove_nvs_erase(const char *key)
{
	if (!key) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
	if (!e) {
		return OVE_ERR_NOT_SUPPORTED;
	}
	memset(e, 0, sizeof(*e));
	return OVE_OK;
}
