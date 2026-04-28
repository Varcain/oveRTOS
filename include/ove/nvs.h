/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_nvs Non-Volatile Storage
 * @ingroup ove_data
 * @brief Key-value store backed by non-volatile memory.
 *
 * Provides a simple string-keyed binary value store persisted in
 * non-volatile storage (e.g. flash, EEPROM). Keys are null-terminated
 * strings; values are arbitrary byte blobs of any size supported by the
 * backend.
 *
 * The subsystem must be initialised once with @ref ove_nvs_init before any
 * read, write, or erase operations are performed.
 *
 * @note Requires @c CONFIG_OVE_NVS.
 * @{
 */

#ifndef OVE_NVS_H
#define OVE_NVS_H

#include "ove/types.h"
#include "ove_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_OVE_NVS

/**
 * @brief Initialise the non-volatile storage subsystem.
 *
 * Must be called once before any other @c ove_nvs_* function. Performs
 * backend-specific mount and integrity checks.
 *
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_NVS.
 */
int ove_nvs_init(void);

/**
 * @brief Deinitialise the non-volatile storage subsystem.
 *
 * Flushes any pending writes and unmounts the NVS backend. After this call
 * all other @c ove_nvs_* functions will fail until @ref ove_nvs_init is
 * called again.
 *
 * @note Requires @c CONFIG_OVE_NVS.
 */
void ove_nvs_deinit(void);

/**
 * @brief Read a value from non-volatile storage by key.
 *
 * Copies the value associated with @p key into @p buf. At most @p buf_len
 * bytes are written. The actual size of the stored value is written to
 * @p out_len when not @c NULL, which allows the caller to detect truncation
 * or to perform a size query (pass @c NULL for @p buf and 0 for @p buf_len).
 *
 * @param[in]  key      Null-terminated key string.
 * @param[out] buf      Buffer to receive the value, or @c NULL for size query.
 * @param[in]  buf_len  Size of @p buf in bytes.
 * @param[out] out_len  Receives the actual stored value length in bytes, or
 *                      @c NULL if not needed.
 * @return OVE_OK on success, @c OVE_ERR_NOT_FOUND if the key does not exist,
 *         or another negative error code on failure.
 * @note Requires @c CONFIG_OVE_NVS.
 */
int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len);

/**
 * @brief Write or update a value in non-volatile storage.
 *
 * Associates @p key with the @p len bytes pointed to by @p data. If the key
 * already exists its value is replaced. The write is committed to
 * non-volatile storage before the function returns.
 *
 * @param[in] key   Null-terminated key string.
 * @param[in] data  Pointer to the value data to store.
 * @param[in] len   Number of bytes to store.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_NVS.
 */
int ove_nvs_write(const char *key, const void *data, size_t len);

/**
 * @brief Delete a key-value pair from non-volatile storage.
 *
 * Permanently removes the entry identified by @p key. Has no effect if the
 * key does not exist.
 *
 * @param[in] key  Null-terminated key string to erase.
 * @return OVE_OK on success, negative error code on failure.
 * @note Requires @c CONFIG_OVE_NVS.
 */
int ove_nvs_erase(const char *key);

#else /* !CONFIG_OVE_NVS */

static inline int ove_nvs_init(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_nvs_deinit(void)
{
}
static inline int ove_nvs_read(const char *key, void *buf, size_t buf_len, size_t *out_len)
{
	(void)key;
	(void)buf;
	(void)buf_len;
	(void)out_len;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_nvs_write(const char *key, const void *data, size_t len)
{
	(void)key;
	(void)data;
	(void)len;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_nvs_erase(const char *key)
{
	(void)key;
	return OVE_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_OVE_NVS */

#ifdef __cplusplus
}
#endif

/** @} */ /* end of ove_nvs group */

#endif /* OVE_NVS_H */
