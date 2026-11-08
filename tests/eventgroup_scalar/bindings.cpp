/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/eventgroup.hpp"
#include <cstdio>
extern "C" unsigned eventgroup_test_clocks(void);
int main()
{
	unsigned checks = 0, failures = 0;
	auto check = [&](bool value) {
		++checks;
		failures += !value;
	};
	ove::EventGroup group;
	check(group.valid());
	check(ove::EventGroup::valid_mask() == UINT32_MAX);
	check(group.set_bits(UINT32_MAX) == UINT32_MAX);
	check(group.clear_bits(1) == UINT32_MAX);
	check(group.get_bits() == UINT32_MAX - 1);
	check(group.clear_bits(ove::EventGroup::valid_mask()) == UINT32_MAX - 1);
	check(group.set_bits(0) == 0 && group.clear_bits(0) == 0);
	auto zero = group.wait_bits(0, 0, std::chrono::nanoseconds(0));
	check(!zero && zero.error() == ove::Error::InvalidParam);
	auto flags = group.wait_bits(1, 4, std::chrono::nanoseconds(0));
	check(!flags && flags.error() == ove::Error::InvalidParam);
	auto deadline = group.wait_bits_until(0, 0, ove::steady_clock::time_point{});
	check(!deadline && deadline.error() == ove::Error::InvalidParam &&
	      eventgroup_test_clocks() == 0);
	auto timeout = group.wait_bits(1, 0, std::chrono::nanoseconds(0));
	check(!timeout && timeout.error() == ove::Error::Timeout);
	std::printf("eventgroup cpp: checks=%u failures=%u\n", checks, failures);
	return failures != 0;
}
