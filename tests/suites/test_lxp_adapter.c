/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * Consumer-side smoke test for the oveRTOS lxp network adapter
 * (backends/common/lxp_ove_adapter.c) — the glue that bridges the module's
 * handle-based lxp_net_ops to the ove_net HAL (posix_net here) and owns the
 * socket-storage pool. The module's own logic is tested in isolation in the lxp
 * repo (lxp/tests, its own POSIX port); this checks only the contract that lives
 * on the CONSUMER side: that the adapter exports a complete, versioned provider
 * and that its open/close bridge reaches the ove_net backend.
 */

#include "../framework/ove_test.h"

#include "lxp/lxp_net_ops.h"
#include "ove/types.h" /* OVE_OK — the ove_net return the adapter forwards */

extern const struct lxp_net_ops g_lxp_host_net_ops;

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
}

/* open -> close through the adapter reaches the ove_net backend (posix_net):
 * exercises the storage-pool slot alloc/free and the ove_socket_open_ex bridge.
 * A UDP socket needs no peer, so this stays hermetic. The back-to-back stream
 * open proves the freed slot is reused (owned-storage lifecycle). */
static void test_adapter_open_close(void **state)
{
	(void)state;
	const struct lxp_net_ops *ops = &g_lxp_host_net_ops;

	lxp_socket_t s = NULL;
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_DGRAM, 0, &s), OVE_OK);
	assert_non_null(s);
	ops->sock_close(s);

	lxp_socket_t t = NULL;
	assert_int_equal(ops->sock_open(LXP_AF_INET, LXP_SOCK_STREAM, 0, &t), OVE_OK);
	assert_non_null(t);
	ops->sock_close(t);
}

int test_lxp_adapter_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_adapter_ops_wired),
		cmocka_unit_test(test_adapter_open_close),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
