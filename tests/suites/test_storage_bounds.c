/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Storage-bounds test suite — canary red-zones.
 *
 * For each ove_*_storage_t we embed the storage slot between two arrays
 * of `CANARY` magic words, call `_init()` through the usual backend path,
 * exercise the primitive's lifecycle, and assert the canaries are still
 * intact at every step.  This catches the class of bug where a backend
 * writes past the declared storage size — the same failure mode that hit
 * us when the STM32 IWDG layout (20 B) diverged from the consumer-visible
 * stub (8 B) and silently corrupted adjacent BSS.
 *
 * Heap-mode builds ignore the caller-supplied storage pointer entirely,
 * so the test is meaningful only under CONFIG_OVE_ZERO_HEAP.  We skip
 * the suite gracefully in heap mode.
 */

#include "../framework/ove_test.h"

#define CANARY 0xDEADBEEFu
#define CANARY_WORDS 4u

#define CANARY_SLOT(T, name)                              \
	struct name##_slot {                              \
		uint32_t pre[CANARY_WORDS];               \
		T storage;                                \
		uint32_t post[CANARY_WORDS];              \
	};                                                \
	static struct name##_slot s_##name = {            \
		.pre = {CANARY, CANARY, CANARY, CANARY},  \
		.post = {CANARY, CANARY, CANARY, CANARY}, \
	}

#ifdef CONFIG_OVE_ZERO_HEAP

static void reset_canaries(uint32_t *pre, uint32_t *post)
{
	for (unsigned i = 0; i < CANARY_WORDS; ++i) {
		pre[i] = CANARY;
		post[i] = CANARY;
	}
}

static void assert_canaries(const char *label, const uint32_t *pre, const uint32_t *post)
{
	for (unsigned i = 0; i < CANARY_WORDS; ++i) {
		if (pre[i] != CANARY) {
			print_error("%s: pre-canary word %u clobbered: 0x%08x\n", label, i,
				    (unsigned)pre[i]);
			assert_int_equal(pre[i], CANARY);
		}
		if (post[i] != CANARY) {
			print_error("%s: post-canary word %u clobbered: 0x%08x\n", label, i,
				    (unsigned)post[i]);
			assert_int_equal(post[i], CANARY);
		}
	}
}

/* ── Per-primitive canary slots ────────────────────────────────────── */

CANARY_SLOT(ove_mutex_storage_t, mtx);
CANARY_SLOT(ove_sem_storage_t, sem);
CANARY_SLOT(ove_event_storage_t, evt);
CANARY_SLOT(ove_condvar_storage_t, cv);
CANARY_SLOT(ove_queue_storage_t, q);
CANARY_SLOT(ove_timer_storage_t, tm);
CANARY_SLOT(ove_eventgroup_storage_t, eg);
CANARY_SLOT(ove_stream_storage_t, strm);
CANARY_SLOT(ove_watchdog_storage_t, wd);

/* Queue/stream backing buffers live outside the canary wrapper — they are
 * caller-provided memory that the backend is expected to write into. The
 * canary guards only the control-block storage slot. */
static uint8_t q_buffer[sizeof(uint32_t) * 4];
static uint8_t strm_buffer[32];

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_storage_mutex(void **state)
{
	(void)state;
	reset_canaries(s_mtx.pre, s_mtx.post);
	ove_mutex_t m = NULL;
	assert_int_equal(ove_mutex_init(&m, &s_mtx.storage), OVE_OK);
	assert_canaries("mutex_init", s_mtx.pre, s_mtx.post);
	assert_int_equal(ove_mutex_lock(m, 0), OVE_OK);
	ove_mutex_unlock(m);
	assert_canaries("mutex_use", s_mtx.pre, s_mtx.post);
	ove_mutex_deinit(m);
	assert_canaries("mutex_deinit", s_mtx.pre, s_mtx.post);
}

static void test_storage_sem(void **state)
{
	(void)state;
	reset_canaries(s_sem.pre, s_sem.post);
	ove_sem_t s = NULL;
	assert_int_equal(ove_sem_init(&s, &s_sem.storage, 1, 4), OVE_OK);
	assert_canaries("sem_init", s_sem.pre, s_sem.post);
	assert_int_equal(ove_sem_take(s, 0), OVE_OK);
	ove_sem_give(s);
	assert_canaries("sem_use", s_sem.pre, s_sem.post);
	ove_sem_deinit(s);
	assert_canaries("sem_deinit", s_sem.pre, s_sem.post);
}

static void test_storage_event(void **state)
{
	(void)state;
	reset_canaries(s_evt.pre, s_evt.post);
	ove_event_t e = NULL;
	assert_int_equal(ove_event_init(&e, &s_evt.storage), OVE_OK);
	assert_canaries("event_init", s_evt.pre, s_evt.post);
	ove_event_deinit(e);
	assert_canaries("event_deinit", s_evt.pre, s_evt.post);
}

static void test_storage_condvar(void **state)
{
	(void)state;
	reset_canaries(s_cv.pre, s_cv.post);
	ove_condvar_t c = NULL;
	assert_int_equal(ove_condvar_init(&c, &s_cv.storage), OVE_OK);
	assert_canaries("condvar_init", s_cv.pre, s_cv.post);
	ove_condvar_deinit(c);
	assert_canaries("condvar_deinit", s_cv.pre, s_cv.post);
}

static void test_storage_queue(void **state)
{
	(void)state;
	reset_canaries(s_q.pre, s_q.post);
	ove_queue_t qh = NULL;
	assert_int_equal(ove_queue_init(&qh, &s_q.storage, q_buffer, sizeof(uint32_t), 4), OVE_OK);
	assert_canaries("queue_init", s_q.pre, s_q.post);
	uint32_t v = 42;
	assert_int_equal(ove_queue_send(qh, &v, 0), OVE_OK);
	uint32_t out = 0;
	assert_int_equal(ove_queue_receive(qh, &out, 0), OVE_OK);
	assert_int_equal(out, 42);
	assert_canaries("queue_use", s_q.pre, s_q.post);
	ove_queue_deinit(qh);
	assert_canaries("queue_deinit", s_q.pre, s_q.post);
}

static void timer_cb_noop(ove_timer_t t, void *ud)
{
	(void)t;
	(void)ud;
}

static void test_storage_timer(void **state)
{
	(void)state;
	reset_canaries(s_tm.pre, s_tm.post);
	ove_timer_t t = NULL;
	assert_int_equal(ove_timer_init(&t, &s_tm.storage, timer_cb_noop, NULL, 1000, 1), OVE_OK);
	assert_canaries("timer_init", s_tm.pre, s_tm.post);
	ove_timer_deinit(t);
	assert_canaries("timer_deinit", s_tm.pre, s_tm.post);
}

static void test_storage_eventgroup(void **state)
{
	(void)state;
	reset_canaries(s_eg.pre, s_eg.post);
	ove_eventgroup_t eg = NULL;
	assert_int_equal(ove_eventgroup_init(&eg, &s_eg.storage), OVE_OK);
	assert_canaries("eventgroup_init", s_eg.pre, s_eg.post);
	(void)ove_eventgroup_set_bits(eg, 0x1);
	assert_canaries("eventgroup_use", s_eg.pre, s_eg.post);
	ove_eventgroup_deinit(eg);
	assert_canaries("eventgroup_deinit", s_eg.pre, s_eg.post);
}

static void test_storage_stream(void **state)
{
	(void)state;
	reset_canaries(s_strm.pre, s_strm.post);
	ove_stream_t st = NULL;
	assert_int_equal(ove_stream_init(&st, &s_strm.storage, strm_buffer, sizeof(strm_buffer), 1),
			 OVE_OK);
	assert_canaries("stream_init", s_strm.pre, s_strm.post);
	ove_stream_deinit(st);
	assert_canaries("stream_deinit", s_strm.pre, s_strm.post);
}

static void test_storage_watchdog(void **state)
{
	(void)state;
	reset_canaries(s_wd.pre, s_wd.post);
	ove_watchdog_t w = NULL;
	int rc = ove_watchdog_init(&w, &s_wd.storage, 5000);
	/* Some stub backends don't support static storage for the watchdog;
	 * those return NOT_SUPPORTED instead of OK. Canaries must still be
	 * untouched in both cases. */
	if (rc != OVE_OK && rc != OVE_ERR_NOT_SUPPORTED) {
		fail_msg("unexpected watchdog_init rc=%d", rc);
	}
	assert_canaries("watchdog_init", s_wd.pre, s_wd.post);
	if (rc == OVE_OK) {
		ove_watchdog_deinit(w);
		assert_canaries("watchdog_deinit", s_wd.pre, s_wd.post);
	}
}

#endif /* CONFIG_OVE_ZERO_HEAP */

/* ── Runner ─────────────────────────────────────────────────────────── */

int test_storage_bounds_run(void)
{
#ifndef CONFIG_OVE_ZERO_HEAP
	printf("  [SKIP] storage_bounds — heap mode ignores caller storage\n");
	return 0;
#else
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_storage_mutex),	   cmocka_unit_test(test_storage_sem),
		cmocka_unit_test(test_storage_event),	   cmocka_unit_test(test_storage_condvar),
		cmocka_unit_test(test_storage_queue),	   cmocka_unit_test(test_storage_timer),
		cmocka_unit_test(test_storage_eventgroup), cmocka_unit_test(test_storage_stream),
		cmocka_unit_test(test_storage_watchdog),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
#endif
}
