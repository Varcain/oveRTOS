/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the public user instrumentation API: name table,
 * point marks and zone begin/end records. The clock and current-thread
 * hooks are stubbed so the emitted records can be checked exactly.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ove_config.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

static int failures;
static uint64_t fake_now;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

uint64_t ove_state_stats_now_us(void)
{
	return ++fake_now;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return 42;
}

static struct ove_trace_record out[64];

static void test_names(void)
{
	struct ove_trace_name_desc names[OVE_TRACE_NAME_MAX];

	CHECK(ove_trace_register_name(7, "draw"));
	CHECK(ove_trace_register_name(9, "flush"));

	size_t n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == 2);
	CHECK(names[0].id == 7 && strcmp(names[0].name, "draw") == 0);
	CHECK(names[1].id == 9 && strcmp(names[1].name, "flush") == 0);

	/* Re-registering an id updates in place instead of appending. */
	CHECK(ove_trace_register_name(7, "draw_frame"));
	n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == 2);
	CHECK(names[0].id == 7 && strcmp(names[0].name, "draw_frame") == 0);

	/* NULL/empty names are ignored and reported as rejected. */
	CHECK(!ove_trace_register_name(11, NULL));
	CHECK(!ove_trace_register_name(11, ""));
	CHECK(ove_trace_names(names, OVE_TRACE_NAME_MAX) == 2);

	/* The table owns the name: the caller may free its buffer right
	 * after the registration. */
	char *heap_name = malloc(strlen("heap.drawn") + 1);
	CHECK(heap_name != NULL);
	if (heap_name != NULL) {
		memcpy(heap_name, "heap.drawn", strlen("heap.drawn") + 1);
		CHECK(ove_trace_register_name(12, heap_name));
		free(heap_name);
		n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
		CHECK(n == 3);
		CHECK(names[2].id == 12 && strcmp(names[2].name, "heap.drawn") == 0);
	}

	/* A longer name is truncated to the wire format's limit. */
	CHECK(ove_trace_register_name(13, "a-very-long-trace-name-that-does-not-fit"));
	n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == 4);
	CHECK(names[3].id == 13);
	CHECK(strlen(names[3].name) == OVE_TRACE_NAME_MAX_LEN);
	/* The table keeps the *prefix*, so the name the wire carries is the
	 * same string the table holds (the wire does not truncate again). */
	CHECK(strncmp(names[3].name, "a-very-long-trace-name-that-does-not-fit",
		      OVE_TRACE_NAME_MAX_LEN) == 0);

	/* A name of exactly the limit is kept whole; one byte more loses only
	 * the last byte. */
	char edge[OVE_TRACE_NAME_MAX_LEN + 2];
	memset(edge, 'n', sizeof(edge));
	edge[OVE_TRACE_NAME_MAX_LEN] = '\0';
	CHECK(ove_trace_register_name(13, edge));
	n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == 4 && names[3].id == 13 && strcmp(names[3].name, edge) == 0);
	edge[OVE_TRACE_NAME_MAX_LEN] = 'm';
	edge[OVE_TRACE_NAME_MAX_LEN + 1] = '\0';
	CHECK(ove_trace_register_name(13, edge));
	n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == 4 && strlen(names[3].name) == OVE_TRACE_NAME_MAX_LEN);
	CHECK(strncmp(names[3].name, edge, OVE_TRACE_NAME_MAX_LEN) == 0);

	/* The table is bounded; the extras are dropped and reported. */
	size_t accepted = 0;
	for (uint16_t i = 100; i < 200; i++)
		if (ove_trace_register_name(i, "x"))
			accepted++;
	CHECK(accepted == OVE_TRACE_NAME_MAX - 4);
	n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == OVE_TRACE_NAME_MAX);
}

static void test_marks_and_zones(void)
{
	ove_trace_ring_drain(out, 64); /* discard anything from earlier */
	fake_now = 0;

	ove_trace_mark(7);
	ove_trace_zone_begin(9);
	ove_trace_zone_end(9);

	size_t n = ove_trace_ring_drain(out, 64);
	CHECK(n == 3);

	CHECK(out[0].kind == OVE_TRACE_KIND_MARK);
	CHECK(out[0].tid == 42);
	CHECK(out[0].code == (uint8_t)((OVE_TRACE_PRIM_USER << 4) | OVE_TRACE_ACT_USER));
	CHECK(out[0].object == 7);
	CHECK(out[0].ts_us == 1);

	CHECK(out[1].code == (uint8_t)((OVE_TRACE_PRIM_USER << 4) | OVE_TRACE_ACT_ZONE_BEGIN));
	CHECK(out[1].object == 9);
	CHECK(out[2].code == (uint8_t)((OVE_TRACE_PRIM_USER << 4) | OVE_TRACE_ACT_ZONE_END));
	CHECK(out[2].object == 9);
	CHECK(out[1].ts_us < out[2].ts_us);
}

int main(void)
{
	test_names();
	test_marks_and_zones();

	if (failures) {
		fprintf(stderr, "test_trace_api: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_trace_api: OK\n");
	return 0;
}
