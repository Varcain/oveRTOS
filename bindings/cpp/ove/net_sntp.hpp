/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @file net_sntp.hpp
 * @brief C++ wrappers for the oveRTOS SNTP client API
 */

#pragma once

#include <ove/net_sntp.h>
#include <ove/error.hpp>

#ifdef CONFIG_OVE_NET_SNTP

namespace ove::sntp
{

/**
 * @namespace ove::sntp
 * @brief C++ wrappers around the oveRTOS SNTP client API.
 */

/**
 * @struct Config
 * @brief SNTP client configuration with C++ defaults.
 */
struct Config {
	const char *server{"pool.ntp.org"}; /**< NTP server hostname. */
	uint64_t timeout_ns{OVE_SEC(5)}; /**< Sync timeout (default 5 seconds). */
};

/**
 * @brief Synchronize with an NTP time server.
 * @param[in] cfg Configuration (defaults: pool.ntp.org, 5s timeout).
 * @return Empty `Result<void>` on success; `unexpected` @ref Error
 *         on failure (`Error::NetDnsFail`, `Error::Timeout`, …).
 */
[[nodiscard]] inline Result<void> sync(const Config &cfg = {}) noexcept
{
	ove_sntp_config_t c{cfg.server, cfg.timeout_ns};
	return from_rc(ove_sntp_sync(&c));
}

/**
 * @brief Get the UTC offset from the last successful sync.
 * @return On success, the UTC offset in microseconds.  On failure,
 *         `unexpected(Error::NotSupported)` if no sync has been
 *         performed yet.
 */
[[nodiscard]] inline Result<int64_t> get_offset_us() noexcept
{
	int64_t offset_us = 0;
	const int rc = ove_sntp_get_offset_us(&offset_us);
	return from_rc(rc, offset_us);
}

/**
 * @brief Get current UTC time in seconds since Unix epoch.
 * @return On success, UTC seconds since the Unix epoch.  On failure,
 *         an `unexpected` @ref Error.
 */
[[nodiscard]] inline Result<uint32_t> get_utc() noexcept
{
	uint32_t utc_s = 0;
	const int rc = ove_sntp_get_utc(&utc_s);
	return from_rc(rc, utc_s);
}

} /* namespace ove::sntp */

#endif /* CONFIG_OVE_NET_SNTP */
