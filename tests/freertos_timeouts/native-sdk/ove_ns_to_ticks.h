/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#define ove_ns_to_ticks ove_test_real_ns_to_ticks
#include OVE_TEST_CONVERSION
#undef ove_ns_to_ticks
void ove_test_conversion(uint64_t, TickType_t);
static inline TickType_t ove_ns_to_ticks(uint64_t ns)
{
	TickType_t ticks = ove_test_real_ns_to_ticks(ns);
	ove_test_conversion(ns, ticks);
	return ticks;
}
