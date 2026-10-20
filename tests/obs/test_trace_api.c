/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ove/trace.h"
#include "ove_trace_ring.h"

uint64_t ove_state_stats_now_us(void)
{
	static uint64_t now;
	return ++now;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return 42;
}

static void test_names(void)
{
	struct ove_trace_name_desc names[OVE_TRACE_NAME_MAX];
	char long_name[] = "0123456789abcdefghijklmnopqrstuv";

	assert(!ove_trace_register_name(7, NULL));
	assert(!ove_trace_register_name(7, ""));
	assert(ove_trace_register_name(7, long_name));
	long_name[0] = 'x';
	assert(ove_trace_register_name(7, "draw"));
	assert(ove_trace_names(names, 1) == 1);
	const char truncated[] = "0123456789abcdefghijklmnopqrstu";
	assert(names[0].id == 7 && memcmp(names[0].name, truncated, sizeof(truncated)) == 0);
	assert(ove_trace_names(names, OVE_TRACE_NAME_MAX) == 2);
	assert(names[1].id == 7 && strcmp(names[1].name, "draw") == 0);

	for (uint16_t i = 2; i < OVE_TRACE_NAME_MAX; i++)
		assert(ove_trace_register_name(100 + i, "x"));
	assert(!ove_trace_register_name(7, "full"));
	assert(!ove_trace_register_name(99, "full"));
	assert(ove_trace_names(names, OVE_TRACE_NAME_MAX) == OVE_TRACE_NAME_MAX);
}

static void test_marks_and_zones(void)
{
	struct ove_trace_record records[4];
	const uint8_t actions[] = {OVE_TRACE_ACT_USER, OVE_TRACE_ACT_ZONE_BEGIN,
				   OVE_TRACE_ACT_ZONE_END};
	const uint16_t ids[] = {7, 9, 9};

	ove_trace_mark(7);
	ove_trace_zone_begin(9);
	ove_trace_zone_end(9);
	assert(ove_trace_ring_drain(records, 4) == 3);
	for (size_t i = 0; i < 3; i++) {
		assert(records[i].kind == OVE_TRACE_KIND_MARK && records[i].tid == 42);
		assert(records[i].code == ((OVE_TRACE_PRIM_USER << 4) | actions[i]));
		assert(records[i].object == ids[i] && records[i].ts_us == i + 1);
	}
}

int main(void)
{
	test_names();
	test_marks_and_zones();
	puts("test_trace_api: OK");
	return 0;
}
