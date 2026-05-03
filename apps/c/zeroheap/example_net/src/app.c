/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C Networking Example — zero-heap mode (placeholder).
 *
 * The full networking test suite (lwIP / socket / HTTP / MQTT clients)
 * requires per-connection dynamic allocation that cannot be expressed
 * with caller-supplied storage alone — sockets, TLS sessions, HTTP
 * response bodies, and MQTT publish payloads all grow at runtime.
 *
 * In production zero-heap deployments the right pattern is to pre-
 * allocate a fixed pool of socket / client storage at file scope via
 * OVE_NETIF_DEFINE_STATIC and friends, then accept a bounded number of
 * concurrent connections from that pool.  See apps/c/heap/example_net/
 * for the full feature exercise; this stub keeps the build matrix
 * complete while pointing operators at the heap variant.
 */

#include "ove/ove.h"

static void net_thread(void *arg)
{
	(void)arg;
	OVE_LOG_INF("net (zero-heap): see apps/c/heap/example_net for the full demo");
	while (1) {
		ove_thread_sleep_ms(10000);
	}
}

OVE_THREAD_DEFINE_STATIC(net_thread_handle, 4096, net_thread, NULL, OVE_PRIO_NORMAL, "net-stub");

void ove_main(void)
{
	OVE_LOG_INF("Networking example (zero-heap mode): stub");
	ove_run();
}
