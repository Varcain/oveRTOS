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

#ifdef CONFIG_OVE_NET_SNTP

namespace ove {

/**
 * @namespace ove::sntp
 * @brief C++ wrappers around the oveRTOS SNTP client API.
 */
namespace sntp {

/**
 * @struct Config
 * @brief SNTP client configuration with C++ defaults.
 */
struct Config {
	const char *server{"pool.ntp.org"};
	uint32_t    timeout_ms{5000};
};

/**
 * @brief Synchronize with an NTP time server.
 * @param[in] cfg Configuration (defaults: pool.ntp.org, 5s timeout).
 * @return `OVE_OK` on success, or a negative error code.
 */
[[nodiscard]] inline int sync(const Config &cfg = {}) {
	ove_sntp_config_t c{cfg.server, cfg.timeout_ms};
	return ove_sntp_sync(&c);
}

/**
 * @brief Get the UTC offset from the last successful sync.
 * @param[out] offset_us UTC offset in microseconds.
 * @return `OVE_OK` on success, `OVE_ERR_NOT_SUPPORTED` if no sync done.
 */
[[nodiscard]] inline int get_offset_us(int64_t &offset_us) {
	return ove_sntp_get_offset_us(&offset_us);
}

/**
 * @brief Get current UTC time in seconds since Unix epoch.
 * @param[out] utc_s UTC seconds.
 * @return `OVE_OK` on success.
 */
[[nodiscard]] inline int get_utc(uint32_t &utc_s) {
	return ove_sntp_get_utc(&utc_s);
}

} /* namespace sntp */

} // namespace ove

#endif /* CONFIG_OVE_NET_SNTP */
