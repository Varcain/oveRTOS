/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Consumer-side smoke tests for the oveRTOS/LXP host adapters. The network
 * adapter owns socket storage and bridges the module's handle API to ove_net.
 * The thread adapter copies snapshots across the two independently owned type
 * contracts without relying on compatible struct layout.
 */

#include "../framework/ove_test.h"

#include "ove_net_ready.h"
#include "lxp_ove_thread_adapter.h"
#include "lxp/lxp_net_ops.h"
#include "ove/types.h" /* OVE_OK — the ove_net return the adapter forwards */

extern const struct lxp_net_ops g_lxp_host_net_ops;
static unsigned g_socket_kicks;

void lxp_sock_kick(void)
{
	g_socket_kicks++;
}

/* The exported adapter table passed to lxp_run must be complete and versioned. */
static void test_adapter_ops_wired(void **state)
{
	(void)state;
	const struct lxp_net_ops *ops = &g_lxp_host_net_ops;
	assert_non_null(ops);
	assert_int_equal(ops->abi_version, LXP_NET_OPS_ABI_VERSION);
	assert_int_equal(ops->struct_size, sizeof(*ops));
	assert_non_null(ops->sock_open);
	assert_non_null(ops->sock_accept);
	assert_non_null(ops->sock_close);
	assert_non_null(ops->sock_connect);
	assert_non_null(ops->sock_bind);
	assert_non_null(ops->sock_listen);
	assert_non_null(ops->sock_send);
	assert_non_null(ops->sock_recv);
	assert_non_null(ops->sock_sendto);
	assert_non_null(ops->sock_recvfrom);
	assert_non_null(ops->sock_set_nonblock);
	assert_non_null(ops->sock_poll);
	assert_non_null(ops->sock_shutdown);
	assert_non_null(ops->sock_getsockname);
	assert_non_null(ops->sock_getpeername);
	assert_non_null(ops->sock_get_error);
	assert_non_null(ops->netif_get_addr);
	assert_non_null(ops->netif_get_hwaddr);
	assert_non_null(ops->netif_get_flags);
	assert_non_null(ops->netif_set_addr);
	assert_non_null(ops->netif_set_up);
	assert_true(ops->capabilities & LXP_NET_CAP_SOCKET_READY_EVENT);
}

/* open -> close through the adapter reaches the ove_net backend (posix_net):
 * exercises the storage-pool slot alloc/free and the ove_socket_open_ex bridge.
 * Readiness remains subscribed until the last concurrently owned socket closes. */
static void test_adapter_open_close(void **state)
{
	(void)state;
	const struct lxp_net_ops *ops = &g_lxp_host_net_ops;

	lxp_socket_t s = NULL;
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_DGRAM, 0, &s), OVE_OK);
	assert_non_null(s);
	lxp_socket_t t = NULL;
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_STREAM, 0, &t), OVE_OK);
	assert_non_null(t);

	g_socket_kicks = 0;
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 1);
	ops->sock_close(s);
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 2);
	ops->sock_close(t);
	ove_net_ready_publish();
	assert_int_equal(g_socket_kicks, 2);
}

static int32_t test_slot_lookup(uintptr_t identity)
{
	return identity == 0x1234u ? 3 : LXP_THREAD_SLOT_NONE;
}

static void test_thread_adapter_copies_contract(void **state)
{
	(void)state;
	const struct ove_thread_info host = {
		.name = "worker",
		.identity = 0x1234u,
		.lxp_slot = 99,
		.state = OVE_THREAD_STATE_BLOCKED,
		.priority = 7,
		.stack_used = 320,
		.stack_size = 1024,
		.cpu_percent_x100 = 1250,
		.state_times = {
			.running_us = 100,
			.ready_us = 200,
			.blocked_us = 300,
			.suspended_us = 400,
		},
	};
	struct lxp_thread_info out = {0};

	lxp_ove_thread_info_copy(&out, &host, test_slot_lookup);

	assert_ptr_equal(out.name, host.name);
	assert_int_equal(out.identity, host.identity);
	assert_int_equal(out.lxp_slot, 3);
	assert_int_equal(out.state, LXP_THREAD_STATE_BLOCKED);
	assert_int_equal(out.priority, host.priority);
	assert_int_equal(out.stack_used, host.stack_used);
	assert_int_equal(out.stack_size, host.stack_size);
	assert_int_equal(out.cpu_percent_x100, host.cpu_percent_x100);
	assert_int_equal(out.state_times.running_us, host.state_times.running_us);
	assert_int_equal(out.state_times.ready_us, host.state_times.ready_us);
	assert_int_equal(out.state_times.blocked_us, host.state_times.blocked_us);
	assert_int_equal(out.state_times.suspended_us, host.state_times.suspended_us);
}

static void test_thread_adapter_maps_unknown_state(void **state)
{
	(void)state;
	const struct ove_thread_info host = {
		.identity = 0x5678u,
		.state = (ove_thread_state_t)99,
	};
	struct lxp_thread_info out = {0};

	lxp_ove_thread_info_copy(&out, &host, test_slot_lookup);

	assert_int_equal(out.lxp_slot, LXP_THREAD_SLOT_NONE);
	assert_int_equal(out.state, LXP_THREAD_STATE_UNKNOWN);
}

int test_lxp_adapter_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_adapter_ops_wired),
		cmocka_unit_test(test_adapter_open_close),
		cmocka_unit_test(test_thread_adapter_copies_contract),
		cmocka_unit_test(test_thread_adapter_maps_unknown_state),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
