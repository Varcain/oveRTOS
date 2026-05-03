/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * oveRTOS C++ Networking Example — zero-heap mode (placeholder).
 *
 * The full networking test suite uses runtime allocation paths inside
 * the lwIP / TLS / HTTP / MQTT clients that cannot be expressed
 * statically.  See apps/cpp/heap/example_net/ for the full demo.
 */

#include <ove/ove.hpp>

static void net_thread(void *)
{
	OVE_LOG_INF("net (zero-heap): see apps/cpp/heap/example_net for the full demo");
	while (true) {
		ove::Thread<0>::sleep_ms(10000);
	}
}

OVE_MAIN()
{
	OVE_LOG_INF("C++ networking example (zero-heap mode): stub");

	static ove::Thread<4096> net(net_thread, nullptr, OVE_PRIO_NORMAL, "net-stub");
	(void)net;

	ove::run();
}
