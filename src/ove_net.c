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
