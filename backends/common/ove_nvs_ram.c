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
static int nvs_key_ok(const char *key)
{
	return ove_nvs_key_is_valid(key) && memchr(key, '\0', NVS_MAX_KEY_LEN) != NULL;
}

static struct nvs_entry *nvs_find(const char *key)
{
	for (int i = 0; i < OVE_NVS_RAM_ENTRIES; i++) {
		if (nvs_store[i].used && strcmp(nvs_store[i].key, key) == 0) {
			return &nvs_store[i];
		}
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

int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	if (!nvs_key_ok(key) || (!buf && buf_len)) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
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

int ove_nvs_write(const char *key, const void *data, size_t len)
{
	if (!nvs_key_ok(key) || (!data && len) || len > NVS_MAX_VAL_LEN) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
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
		memcpy(e->key, key, strlen(key) + 1); /* fits: nvs_key_ok() */
		e->used = 1;
	}
	if (len) {
		memcpy(e->value, data, len);
	}
	e->len = len;
	return OVE_OK;
}

int ove_nvs_erase(const char *key)
{
	if (!nvs_key_ok(key)) {
		return OVE_ERR_INVALID_PARAM;
	}
	struct nvs_entry *e = nvs_find(key);
	if (e) {
		memset(e, 0, sizeof(*e));
	}
	return OVE_OK;
}
