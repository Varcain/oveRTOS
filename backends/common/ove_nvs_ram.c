/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * NVS kept in a RAM table, for targets with no storage device: the QEMU
 * MPS2-AN500 board on every engine, and the host and emulator test builds.
 * Values last until the program exits, not across runs. Keys follow the same
 * rules as the file-backed NVS, so code that works here works there.
 */

#include "ove/nvs.h"
#include "ove_backend_common.h"
#include <string.h>

/* The NVS suite holds about ten keys at once; 32 entries (about 10.5 KB)
 * keep the RAM-tight STM32F746 Renode images inside their 256 KB. */
#ifndef OVE_NVS_RAM_ENTRIES
#define OVE_NVS_RAM_ENTRIES 32
#endif
#define NVS_MAX_KEY_LEN 64
#define NVS_MAX_VAL_LEN 256

struct nvs_entry {
	char key[NVS_MAX_KEY_LEN];
	uint8_t value[NVS_MAX_VAL_LEN];
	size_t len;
	int used;
};

static struct nvs_entry nvs_store[OVE_NVS_RAM_ENTRIES];

/* A key must fit whole: truncating it would let two keys share an entry. */
size_t ove_nvs_key_max_len(void)
{
	return NVS_MAX_KEY_LEN - 1;
}

static struct nvs_entry *nvs_find(const char *key, size_t len)
{
	for (int i = 0; i < OVE_NVS_RAM_ENTRIES; i++) {
		if (nvs_store[i].used && strlen(nvs_store[i].key) == len &&
		    memcmp(nvs_store[i].key, key, len) == 0)
			return &nvs_store[i];
	}
	return NULL;
}

int ove_nvs_init(void)
{
	return OVE_OK;
}

void ove_nvs_deinit(void)
{
}

int ove_nvs_read_n(const char *key, size_t key_len, void *buf, size_t buf_len, size_t *out_len)
{
	if (!(key_len <= ove_nvs_key_max_len() && ove_nvs_key_is_valid_n(key, key_len)) ||
	    (!buf && buf_len)) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key, key_len);
	if (!e) {
		return OVE_ERR_NOT_FOUND;
	}
	size_t copy = e->len < buf_len ? e->len : buf_len;
	if (copy) {
		memcpy(buf, e->value, copy);
	}
	if (out_len) {
		*out_len = e->len;
	}
	return OVE_OK;
}

int ove_nvs_write_n(const char *key, size_t key_len, const void *data, size_t len)
{
	if (!(key_len <= ove_nvs_key_max_len() && ove_nvs_key_is_valid_n(key, key_len)) ||
	    (!data && len) || len > NVS_MAX_VAL_LEN) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key, key_len);
	if (!e) {
		for (int i = 0; i < OVE_NVS_RAM_ENTRIES; i++) {
			if (!nvs_store[i].used) {
				e = &nvs_store[i];
				break;
			}
		}
		if (!e) {
			return OVE_ERR_NO_MEMORY;
		}
		memcpy(e->key, key, key_len);
		e->key[key_len] = 0;
		e->used = 1;
	}
	if (len) {
		memcpy(e->value, data, len);
	}
	e->len = len;
	return OVE_OK;
}

int ove_nvs_erase_n(const char *key, size_t key_len)
{
	if (!(key_len <= ove_nvs_key_max_len() && ove_nvs_key_is_valid_n(key, key_len))) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key, key_len);
	if (e) {
		memset(e, 0, sizeof(*e));
	}
	return OVE_OK;
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
