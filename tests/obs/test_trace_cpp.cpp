/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#undef NDEBUG
#include <cassert>
#include <cstring>
#include <type_traits>
#include <utility>

#include <ove/trace.hpp>
#include "ove_trace_ring.h"

extern "C" uint64_t ove_state_stats_now_us(void)
{
	return 1;
}

extern "C" uintptr_t ove_backend_thread_current_handle(void)
{
	return 42;
}

int main()
{
	using namespace ove::trace;
	static_assert(std::is_nothrow_destructible_v<Zone> &&
		      std::is_nothrow_move_constructible_v<Zone>);
	static_assert(!std::is_copy_constructible_v<Zone> && !std::is_copy_assignable_v<Zone>);

	ove_trace_name_desc entry{};
	assert(name(7, "draw"));
	assert(ove_trace_names(&entry, 1) == 1);
	assert(entry.id == 7 && std::strcmp(entry.name, "draw") == 0);
	mark(7);
	{
		Zone zone{8};
	}
	{
		Zone from{9};
		Zone to{std::move(from)};
	}
	zone_begin(10);
	zone_end(10);

	constexpr auto begin = OVE_TRACE_ACT_ZONE_BEGIN;
	constexpr auto end = OVE_TRACE_ACT_ZONE_END;
	const unsigned actions[] = {OVE_TRACE_ACT_USER, begin, end, begin, end, begin, end};
	const unsigned ids[] = {7, 8, 8, 9, 9, 10, 10};
	ove_trace_record records[8];
	assert(ove_trace_ring_drain(records, 8) == 7);
	for (size_t i = 0; i < 7; ++i) {
		assert(records[i].kind == OVE_TRACE_KIND_MARK && records[i].tid == 42);
		assert(records[i].code == ((OVE_TRACE_PRIM_USER << 4) | actions[i]));
		assert(records[i].object == ids[i]);
	}
}
