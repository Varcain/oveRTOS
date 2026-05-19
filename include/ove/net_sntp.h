/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#ifndef OVE_NET_SNTP_H
#define OVE_NET_SNTP_H

/**
 * @file net_sntp.h
 * @defgroup ove_net_sntp SNTP Client
 * @brief Simple NTP client for time synchronization.
 *
 * Sends a single NTP query to a time server and stores the UTC offset
 * relative to the monotonic clock.  Useful for wall-clock timestamps,
 * TLS certificate validation, and log correlation.
 *
 * @note Requires @c CONFIG_OVE_NET_SNTP (implies @c CONFIG_OVE_NET
 *       and @c CONFIG_OVE_TIME).
 * @{
 */

#include "ove/types.h"
#include "ove_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SNTP client configuration.
 */
typedef struct {
	const char *server;  /**< NTP server hostname (e.g. "pool.ntp.org"). */
	uint64_t timeout_ns; /**< Query timeout in nanoseconds (0 = 5 seconds). */
} ove_sntp_config_t;

#ifdef CONFIG_OVE_NET_SNTP

/**
 * @brief Synchronize with an NTP server.
 *
 * Sends a single NTP request and stores the computed UTC offset.
 * Subsequent calls update the stored offset.
 *
 * @param[in] cfg Configuration (NULL for defaults: pool.ntp.org, 5s timeout).
 * @return OVE_OK on success, negative error code on failure.
 */
int ove_sntp_sync(const ove_sntp_config_t *cfg);

/**
 * @brief Get the UTC offset computed by the last successful sync.
 *
 * The offset can be added to `ove_time_get_us()` to approximate
 * wall-clock time (microseconds since Unix epoch).
 *
 * @param[out] offset_us UTC offset in microseconds.
 * @return OVE_OK if a sync has been performed, OVE_ERR_NOT_SUPPORTED otherwise.
 */
int ove_sntp_get_offset_us(int64_t *offset_us);

/**
 * @brief Get the current UTC time in seconds since Unix epoch.
 *
 * Convenience function: returns monotonic time + NTP offset.
 *
 * @param[out] utc_s UTC seconds since 1970-01-01 00:00:00.
 * @return OVE_OK on success, OVE_ERR_NOT_SUPPORTED if no sync done.
 */
int ove_sntp_get_utc(uint32_t *utc_s);

#else /* !CONFIG_OVE_NET_SNTP */

/** @cond INTERNAL */
static inline int ove_sntp_sync(const ove_sntp_config_t *cfg)
{
	(void)cfg;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_sntp_get_offset_us(int64_t *o)
{
	(void)o;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_sntp_get_utc(uint32_t *u)
{
	(void)u;
	return OVE_ERR_NOT_SUPPORTED;
}
/** @endcond */

#endif /* CONFIG_OVE_NET_SNTP */

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_NET_SNTP_H */
