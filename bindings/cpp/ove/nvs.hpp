/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file nvs.hpp
 * @brief Non-volatile key-value storage functions
 */

#pragma once

#include <ove/nvs.h>
#include <ove/types.hpp>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_NVS

namespace ove::nvs
{

/**
 * @namespace ove::nvs
 * @brief Thin C++ wrappers around the oveRTOS non-volatile storage API.
 *
 * Available when `CONFIG_OVE_NVS` is enabled.  Keys are null-terminated
 * strings; values are arbitrary byte buffers.
 */

/**
 * @brief Initialises the NVS subsystem.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> init() noexcept
{
	return from_rc(ove_nvs_init());
}

/**
 * @brief Deinitialises the NVS subsystem and frees associated resources.
 */
inline void deinit()
{
	ove_nvs_deinit();
}

/**
 * @brief Reads the value associated with a key from NVS.
 * @param[in]  key The null-terminated key string.
 * @param[out] buf Buffer to receive the stored value.
 * @param[in]  len Size of `buf` in bytes.
 * @return On success, the number of bytes actually read.  On
 *         failure, an `unexpected` @ref Error (`Error::NotFound`,
 *         …).
 */
[[nodiscard]] inline Result<size_t> read(const char *key, void *buf, size_t len) noexcept
{
	size_t out = 0;
	const int rc = ove_nvs_read(key, buf, len, &out);
	return from_rc(rc, out);
}

/**
 * @brief Writes a value associated with a key to NVS.
 * @param[in] key  The null-terminated key string.
 * @param[in] data Pointer to the data to store.
 * @param[in] len  Number of bytes to write.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure.
 */
[[nodiscard]] inline Result<void> write(const char *key, const void *data, size_t len) noexcept
{
	return from_rc(ove_nvs_write(key, data, len));
}

/**
 * @brief Erases the value associated with a key from NVS.
 * @param[in] key The null-terminated key string.
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure (`Error::NotFound` if no such key).
 */
[[nodiscard]] inline Result<void> erase(const char *key) noexcept
{
	return from_rc(ove_nvs_erase(key));
}

} /* namespace ove::nvs */

#endif /* CONFIG_OVE_NVS */
