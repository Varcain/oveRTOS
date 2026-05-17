/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Renode-target networking suite — exercises ove_netif_* and ove_socket_*
 * APIs against Renode's modelled SynopsysEthernetMAC + EthernetPhysicalLayer.
 *
 * Skipped on every other target.  No external network server is required:
 * the tests verify bring-up (lwIP init, netif up/down, IP assignment) and
 * lwIP-loopback round-trip (LWIP_NETIF_LOOPBACK is enabled in lwipopts.h),
 * both of which catch the bugs we care about — driver descriptor wiring,
 * sys_arch port correctness, MAC clock/reset gating — without needing a
 * Renode-side network simulator.
 */

#include "../framework/ove_test.h"
#include "../framework/renode_obs.h"
#include "ove/net.h"

#include <stdio.h>
#include <string.h>

#if OVE_OBS_AVAILABLE && defined(CONFIG_OVE_NET)

static void test_renode_netif_static_up(void **state)
{
	(void)state;

	static ove_netif_storage_t storage;
	ove_netif_t netif = NULL;
	int rc = ove_netif_init(&netif, &storage);
	assert_int_equal(rc, OVE_OK);
	assert_non_null(netif);

	ove_netif_config_t cfg = {0};
	cfg.use_dhcp = 0;
	cfg.static_ip.family = OVE_AF_INET;
	cfg.static_ip.addr[0] = 192;
	cfg.static_ip.addr[1] = 0;
	cfg.static_ip.addr[2] = 2;
	cfg.static_ip.addr[3] = 15;
	cfg.netmask.family = OVE_AF_INET;
	cfg.netmask.addr[0] = 255;
	cfg.netmask.addr[1] = 255;
	cfg.netmask.addr[2] = 255;
	cfg.netmask.addr[3] = 0;
	cfg.gateway.family = OVE_AF_INET;
	cfg.gateway.addr[0] = 192;
	cfg.gateway.addr[1] = 0;
	cfg.gateway.addr[2] = 2;
	cfg.gateway.addr[3] = 1;
	rc = ove_netif_up(netif, &cfg);
	assert_int_equal(rc, OVE_OK);

	/* Read back the IP — verifies the netif was actually configured by
	 * lwIP, not just that ove_netif_up returned OK. */
	ove_sockaddr_t ip = {0}, gw = {0}, nm = {0};
	rc = ove_netif_get_addr(netif, &ip, &gw, &nm);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(ip.addr[0], 192);
	assert_int_equal(ip.addr[3], 15);

	/* Don't tear down — subsequent tests reuse the brought-up stack. */
}

static void test_renode_udp_loopback(void **state)
{
	(void)state;

	/* lwIP's loopback netif (LWIP_NETIF_LOOPBACK = 1) lets us round-trip
	 * a UDP packet without the Synopsys MAC being on a real wire.  This
	 * exercises the ove_socket_* API end-to-end through lwIP_socket and
	 * the FreeRTOS sys_arch port. */
	static ove_socket_storage_t tx_storage, rx_storage;
	ove_socket_t tx = NULL, rx = NULL;

	int rc = ove_socket_open(&rx, &rx_storage, OVE_AF_INET, OVE_SOCK_DGRAM);
	assert_int_equal(rc, OVE_OK);
	rc = ove_socket_open(&tx, &tx_storage, OVE_AF_INET, OVE_SOCK_DGRAM);
	assert_int_equal(rc, OVE_OK);

	ove_sockaddr_t bind_addr = {.family = OVE_AF_INET, .port = 4242};
	bind_addr.addr[0] = 127; /* 127.0.0.1 */
	bind_addr.addr[3] = 1;
	rc = ove_socket_bind(rx, &bind_addr);
	assert_int_equal(rc, OVE_OK);

	const char payload[] = "renode-net";
	ove_sockaddr_t dst = bind_addr;
	size_t sent = 0;
	rc = ove_socket_sendto(tx, payload, sizeof(payload) - 1, &sent, &dst);
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(sent, sizeof(payload) - 1);

	uint8_t buf[32] = {0};
	ove_sockaddr_t from = {0};
	size_t received = 0;
	rc = ove_socket_recvfrom(rx, buf, sizeof(buf), &received, &from, OVE_MS(500));
	assert_int_equal(rc, OVE_OK);
	assert_int_equal(received, sizeof(payload) - 1);
	assert_memory_equal(buf, payload, sizeof(payload) - 1);

	ove_socket_close(tx);
	ove_socket_close(rx);
}

#endif /* OVE_OBS_AVAILABLE && CONFIG_OVE_NET */

int test_renode_stm32_net_run(void)
{
#if !OVE_OBS_AVAILABLE || !defined(CONFIG_OVE_NET)
	printf("  [SKIP] renode_stm32_net — non-Renode or net disabled\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_renode_netif_static_up),
		cmocka_unit_test(test_renode_udp_loopback),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
