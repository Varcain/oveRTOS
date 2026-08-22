/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Application-owned network readiness reporting and TCP smoke workload.
 */

#include "network_smoke.h"

#include "ove_config.h"
#include "ove/lxp_console.h"

#if defined(CONFIG_OVE_LINUX_NET)
#include <stddef.h>

#include "ove/net.h"
#include "ove/thread.h"
#include "ove/time.h"
#endif

void linux_interop_network_report(const ove_lxp_host_t *host)
{
#if defined(CONFIG_OVE_LINUX_NET)
	ove_sockaddr_t ip = {0}, gateway = {0};
	if (ove_lxp_host_netif_get_addr(host, &ip, &gateway, NULL) == OVE_OK) {
		ove_lxp_console_printf("[demo] eth0 up ip=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
				       (unsigned int)ip.addr[0], (unsigned int)ip.addr[1],
				       (unsigned int)ip.addr[2], (unsigned int)ip.addr[3],
				       (unsigned int)gateway.addr[0], (unsigned int)gateway.addr[1],
				       (unsigned int)gateway.addr[2],
				       (unsigned int)gateway.addr[3]);
	} else {
		ove_lxp_console_write("[demo] eth0 address unavailable after bring-up\n");
	}
#else
	(void)host;
#endif
}

void linux_interop_network_smoke(const ove_lxp_host_t *host)
{
#if defined(CONFIG_OVE_LINUX_NET)
	static ove_socket_storage_t socket_storage;
	ove_sockaddr_t peer = {0};
	if (ove_lxp_host_netif_get_addr(host, NULL, &peer, NULL) != OVE_OK) {
		ove_lxp_console_write("[demo] socket smoke: configured gateway unavailable\n");
		return;
	}
	peer.port = 22;
	const uint64_t started_ns = ove_time_now_steady_ns();
	int last_rc = -1;
	for (uint32_t attempt = 1; attempt <= 12; attempt++) {
		ove_socket_t socket = NULL;
		last_rc = ove_socket_open(&socket, &socket_storage, OVE_AF_INET, OVE_SOCK_STREAM);
		if (last_rc != OVE_OK)
			goto retry;

		last_rc = ove_socket_connect(socket, &peer, OVE_MS(500));
		if (last_rc != OVE_OK) {
			ove_socket_close(socket);
			goto retry;
		}

		char reply[80];
		size_t received = 0;
		last_rc = ove_socket_recv(socket, reply, sizeof(reply) - 1, &received, OVE_SEC(1));
		if (last_rc == OVE_OK && received > 0) {
			for (size_t i = 0; i < received; i++)
				if (reply[i] == '\r' || reply[i] == '\n')
					reply[i] = 0;
			reply[received < sizeof(reply) ? received : sizeof(reply) - 1] = 0;
			ove_lxp_console_printf(
				"[demo] socket smoke (post-phase1) OK <- %u.%u.%u.%u:22: %s "
				"(ready after %u ms, attempt %u)\n",
				(unsigned int)peer.addr[0], (unsigned int)peer.addr[1],
				(unsigned int)peer.addr[2], (unsigned int)peer.addr[3], reply,
				(unsigned int)((ove_time_now_steady_ns() - started_ns) / OVE_MS(1)),
				(unsigned int)attempt);
			ove_socket_close(socket);
			return;
		}
		ove_socket_close(socket);

	retry:
		if (attempt < 12)
			ove_thread_sleep_ms(250);
	}

	ove_lxp_console_printf(
		"[demo] socket smoke (post-phase1): not ready after %u ms, last rc=%d\n",
		(unsigned int)((ove_time_now_steady_ns() - started_ns) / OVE_MS(1)), last_rc);
#else
	(void)host;
#endif
}
