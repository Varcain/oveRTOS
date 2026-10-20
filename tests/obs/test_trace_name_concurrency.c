/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Concurrency regression test for the user trace name table.
 *
 * The table is written by the application thread (ove_trace_register_name)
 * and read by the sim pump thread (ove_trace_names).  The snapshot the
 * reader gets must never contain a half-written entry: every name it
 * observes has to be exactly one of the values the writer published.
 *
 * The writer alternates one id between two 31-byte names that differ in
 * every byte (all-'A' vs all-'B') and then appends fresh ids, so a torn
 * copy (a mix of the old and the new bytes, or a name that is only
 * partially written) is detectable.
 *
 * This is the property ThreadSanitizer cannot check: TSan only reports
 * the data race, not the value the reader ends up with.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#include "ove_config.h"
#include "ove/trace.h"
#include "ove_trace_ring.h"

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);                      \
		}                                                                                \
	} while (0)

/* The trace backend pulls these in through ove/thread_state_stats.h and
 * ove/thread.h; the name table itself needs neither. */
uint64_t ove_state_stats_now_us(void)
{
	return 0;
}

uintptr_t ove_backend_thread_current_handle(void)
{
	return 1;
}

/* Ids the writer re-registers: the name alternates between two values
 * that differ in every byte, so a torn copy is unambiguous. */
#define TOGGLED_ID 1
#define TOGGLE_LEN OVE_TRACE_NAME_MAX_LEN

/* Ids that are registered once and never change. */
#define STABLE_ID_BASE 10
#define STABLE_ID_COUNT 8

/* Ids appended by the writer while the reader is running. */
#define APPEND_ID_BASE 100
/* The table already holds the stable ids and the toggled one. */
#define APPEND_COUNT (OVE_TRACE_NAME_MAX - STABLE_ID_COUNT - 1)

/* Toggles per burst, and the number of bursts.  The pause between the
 * bursts gives the reader a chance to take a clean snapshot; without it
 * the reader legitimately starves (the snapshot is retried, not torn). */
#define TOGGLE_BURSTS 200
#define TOGGLE_PER_BURST 50
#define TOGGLE_PAUSE_NS 20000

static int writer_done;

static void fill_name(char *buf, size_t len, char c)
{
	memset(buf, c, len);
	buf[len] = '\0';
}

/* Every byte of the name must be @c. */
static int name_is_all(const char *name, char c, size_t len)
{
	for (size_t i = 0; i < len; i++)
		if (name[i] != c)
			return 0;
	return 1;
}

static void *writer_thread(void *arg)
{
	(void)arg;
	char buf[OVE_TRACE_NAME_MAX_LEN + 1];

	for (int burst = 0; burst < TOGGLE_BURSTS; burst++) {
		for (int i = 0; i < TOGGLE_PER_BURST; i++) {
			fill_name(buf, OVE_TRACE_NAME_MAX_LEN, (i & 1) ? 'B' : 'A');
			if (!ove_trace_register_name(TOGGLED_ID, buf)) {
				__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
				fprintf(stderr, "register(%d) failed\n", TOGGLED_ID);
				__atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
				return NULL;
			}
		}
		struct timespec ts = {0, TOGGLE_PAUSE_NS};
		nanosleep(&ts, NULL);
	}

	for (int i = 0; i < APPEND_COUNT; i++) {
		fill_name(buf, OVE_TRACE_NAME_MAX_LEN, 'C');
		if (!ove_trace_register_name((uint16_t)(APPEND_ID_BASE + i), buf)) {
			__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
			fprintf(stderr, "register(%d) failed\n", APPEND_ID_BASE + i);
			break;
		}
	}

	__atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
	return NULL;
}

int main(void)
{
	struct ove_trace_name_desc names[OVE_TRACE_NAME_MAX];

	/* Stable ids, registered before the reader starts. */
	for (int i = 0; i < STABLE_ID_COUNT; i++) {
		char buf[32];
		snprintf(buf, sizeof(buf), "stable-%d", STABLE_ID_BASE + i);
		CHECK(ove_trace_register_name((uint16_t)(STABLE_ID_BASE + i), buf));
	}
	/* The toggled id starts out as the 'A' variant. */
	CHECK(ove_trace_register_name(TOGGLED_ID,
				      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));

	pthread_t writer;
	CHECK(pthread_create(&writer, NULL, writer_thread, NULL) == 0);

	unsigned long snapshots = 0;
	unsigned long empty = 0;
	while (!__atomic_load_n(&writer_done, __ATOMIC_ACQUIRE)) {
		size_t n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
		if (n == 0) {
			/* The reader gave up on this attempt: no snapshot is
			 * reported, and the caller keeps the previous names. */
			empty++;
			continue;
		}
		snapshots++;

		for (size_t i = 0; i < n; i++) {
			/* The name is always NUL-terminated inside the
			 * descriptor. */
			CHECK(memchr(names[i].name, '\0', OVE_TRACE_NAME_MAX_LEN + 1) != NULL);

			int bad = 0;
			if (names[i].id == TOGGLED_ID) {
				/* Old or new value, never a mix. */
				bad = !name_is_all(names[i].name, 'A', TOGGLE_LEN) &&
				      !name_is_all(names[i].name, 'B', TOGGLE_LEN);
			} else if (names[i].id >= STABLE_ID_BASE &&
				   names[i].id < STABLE_ID_BASE + STABLE_ID_COUNT) {
				char buf[32];
				snprintf(buf, sizeof(buf), "stable-%u", names[i].id);
				bad = strcmp(names[i].name, buf) != 0;
			} else if (names[i].id >= APPEND_ID_BASE &&
				   names[i].id < APPEND_ID_BASE + APPEND_COUNT) {
				/* An appended entry is published only when it
				 * is complete. */
				bad = !name_is_all(names[i].name, 'C', TOGGLE_LEN);
			} else {
				bad = 1; /* unexpected id */
			}
			if (bad) {
				fprintf(stderr, "torn or unexpected entry: id=%u name='%.*s'\n",
					names[i].id, (int)OVE_TRACE_NAME_MAX_LEN, names[i].name);
				__atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
			}
		}
	}

	pthread_join(writer, NULL);

	/* One last, quiescent snapshot: the table must be complete. */
	size_t n = ove_trace_names(names, OVE_TRACE_NAME_MAX);
	CHECK(n == OVE_TRACE_NAME_MAX);
	CHECK(snapshots > 0);

	printf("test_trace_name_concurrency: %lu snapshots, %lu empty\n", snapshots, empty);
	if (failures) {
		fprintf(stderr, "test_trace_name_concurrency: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_trace_name_concurrency: OK\n");
	return 0;
}
