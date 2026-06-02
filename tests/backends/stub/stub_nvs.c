/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Stub NVS backend for bare-metal testing.
 * Same as posix_nvs — uses only stdlib/string.
 */

#include "ove/ove.h"
#include <stdlib.h>
#include <string.h>

/* 32 entries (×328 B ≈ 10.5 KB) — the NVS suite stores at most ~4
 * concurrent keys, so 64 was 16× over-provisioned.  This stub is linked
 * into the RAM-tight STM32F746 Renode firmwares (256 KB main RAM); 64
 * entries left no headroom once the freertos/zephyr stream storage grew
 * (ring + two static semaphores for exact `trigger` parity), tipping
 * .bss over the region.  32 keeps 8× headroom over real test usage. */
#define NVS_MAX_ENTRIES 32
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
		return OVE_ERR_NOT_FOUND;
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
