/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Simple NTP client (RFC 4330 / SNTPv4).
 *
 * Sends a single mode-3 (client) request to an NTP server on UDP port
 * 123 and extracts the transmit timestamp to compute a UTC offset.
 * All I/O uses the oveRTOS socket API — no platform-specific code.
 */

#include "ove/ove.h"
#include "ove/net_sntp.h"

#include <string.h>

#ifdef CONFIG_OVE_NET_SNTP

/* NTP epoch is 1900-01-01, Unix epoch is 1970-01-01 */
#define NTP_UNIX_DELTA 2208988800ULL

/* NTP packet is 48 bytes */
#define NTP_PKT_LEN 48

/* Default server and timeout */
#define SNTP_DEFAULT_SERVER "pool.ntp.org"
#define SNTP_DEFAULT_TIMEOUT_NS OVE_SEC(5)

/* Stored offset from last sync */
static int64_t s_offset_us;
static int s_synced;

int ove_sntp_sync(const ove_sntp_config_t *cfg)
{
	const char *server = SNTP_DEFAULT_SERVER;
	uint64_t timeout = SNTP_DEFAULT_TIMEOUT_NS;

	if (cfg) {
		if (cfg->server)
			server = cfg->server;
		if (cfg->timeout_ns > 0)
			timeout = cfg->timeout_ns;
	}

	/* Resolve server hostname */
	ove_sockaddr_t srv_addr;
	int ret = ove_dns_resolve(server, &srv_addr, timeout);
	if (ret != OVE_OK)
		return ret;
	srv_addr.port = 123; /* NTP port */

	/* Open UDP socket */
	ove_socket_t sock;
	ove_socket_storage_t sock_storage;
	ret = ove_socket_open(&sock, &sock_storage, OVE_AF_INET, OVE_SOCK_DGRAM);
	if (ret != OVE_OK)
		return ret;

	/* Build NTP request (mode 3 = client, version 4) */
	uint8_t pkt[NTP_PKT_LEN];
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = (4 << 3) | 3; /* LI=0, VN=4, Mode=3 (client) */

	/* Record local time before sending */
	uint64_t t1_us = 0;
	ove_time_get_us(&t1_us);

	/* Send request */
	size_t sent = 0;
	ret = ove_socket_sendto(sock, pkt, NTP_PKT_LEN, &sent, &srv_addr);
	if (ret != OVE_OK) {
		ove_socket_close(sock);
		return ret;
	}

	/* Receive response */
	size_t received = 0;
	ret = ove_socket_recvfrom(sock, pkt, NTP_PKT_LEN, &received, NULL, timeout);
	ove_socket_close(sock);

	if (ret != OVE_OK)
		return ret;
	if (received < NTP_PKT_LEN)
		return OVE_ERR_NOT_SUPPORTED;

	/* Record local time after receiving */
	uint64_t t4_us = 0;
	ove_time_get_us(&t4_us);

	/*
	 * Extract transmit timestamp (bytes 40-47).
	 * NTP timestamp: 32-bit seconds + 32-bit fraction (since 1900-01-01).
	 */
	uint32_t ntp_secs = ((uint32_t)pkt[40] << 24) | ((uint32_t)pkt[41] << 16) |
			    ((uint32_t)pkt[42] << 8) | ((uint32_t)pkt[43]);
	uint32_t ntp_frac = ((uint32_t)pkt[44] << 24) | ((uint32_t)pkt[45] << 16) |
			    ((uint32_t)pkt[46] << 8) | ((uint32_t)pkt[47]);

	if (ntp_secs == 0)
		return OVE_ERR_NOT_SUPPORTED; /* kiss-of-death or invalid */

	/* Guard against era-0 wrap and pre-1970 timestamps — the subtraction
	 * below is on unsigned types and would wrap to a huge value. */
	if (ntp_secs < NTP_UNIX_DELTA)
		return OVE_ERR_NOT_SUPPORTED;

	/* Convert NTP timestamp to Unix microseconds */
	uint64_t unix_secs = (uint64_t)ntp_secs - NTP_UNIX_DELTA;
	uint64_t frac_us = ((uint64_t)ntp_frac * 1000000ULL) >> 32;
	uint64_t ntp_us = unix_secs * 1000000ULL + frac_us;

	/*
	 * Simple offset calculation using the midpoint of local time.
	 * For SNTP accuracy this is sufficient (~ms precision).
	 * offset = ntp_time - local_midpoint
	 */
	uint64_t local_mid_us = t1_us + (t4_us - t1_us) / 2;
	s_offset_us = (int64_t)(ntp_us - local_mid_us);
	s_synced = 1;

	return OVE_OK;
}

int ove_sntp_get_offset_us(int64_t *offset_us)
{
	if (!s_synced || !offset_us)
		return OVE_ERR_NOT_SUPPORTED;
	*offset_us = s_offset_us;
	return OVE_OK;
}

int ove_sntp_get_utc(uint32_t *utc_s)
{
	if (!s_synced || !utc_s)
		return OVE_ERR_NOT_SUPPORTED;

	uint64_t local_us = 0;
	ove_time_get_us(&local_us);

	int64_t utc_us = (int64_t)local_us + s_offset_us;
	*utc_s = (uint32_t)(utc_us / 1000000ULL);

	return OVE_OK;
}

#endif /* CONFIG_OVE_NET_SNTP */
