/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/net.h"

#include <string.h>

void ove_sockaddr_ipv4(ove_sockaddr_t *addr, uint8_t a, uint8_t b, uint8_t c, uint8_t d,
		       uint16_t port)
{
	if (!addr)
		return;
	memset(addr, 0, sizeof(*addr));
	addr->family = OVE_AF_INET;
	addr->port = port;
	addr->addr[0] = a;
	addr->addr[1] = b;
	addr->addr[2] = c;
	addr->addr[3] = d;
}

int ove_sockaddr_parse_ipv4(ove_sockaddr_t *addr, const char *text, uint16_t port)
{
	if (!addr || !text)
		return OVE_ERR_INVALID_PARAM;
	uint8_t octets[4];
	for (unsigned octet = 0; octet < 4u; octet++) {
		unsigned value = 0u;
		unsigned digits = 0u;
		while (*text >= '0' && *text <= '9') {
			value = value * 10u + (unsigned)(*text - '0');
			if (value > 255u)
				return OVE_ERR_INVALID_PARAM;
			text++;
			digits++;
		}
		if (digits == 0u || (octet < 3u ? *text != '.' : *text != '\0'))
			return OVE_ERR_INVALID_PARAM;
		octets[octet] = (uint8_t)value;
		if (octet < 3u)
			text++;
	}
	ove_sockaddr_ipv4(addr, octets[0], octets[1], octets[2], octets[3], port);
	return OVE_OK;
}
