/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "provider.h"
#include "ove/sync.hpp"
#include <atomic>
#include <thread>

int main()
{
	fixture_binding_start();
	std::atomic<unsigned> completed{0};
	{
		ove::Event event;
		auto consume = [&] {
			event.wait();
			completed.fetch_add(1, std::memory_order_relaxed);
		};
		std::thread first(consume), second(consume);
		fixture_ready_count(2);
		for (unsigned i = 0; i < 2; ++i) {
			fixture_prepare_signal();
			event.signal();
			fixture_await_signal();
		}
		first.join();
		second.join();
		fixture_check(completed.load(std::memory_order_relaxed) == 2,
			      "public C++ shared waits complete before owner destruction");
	}
	return fixture_binding_report("cpp");
}
