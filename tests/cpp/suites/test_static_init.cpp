#include "../framework/ove_test.hpp"
#include <cstdio>

/*
 * Tests for C++ constructor-based initialization.
 * These verify that each RAII class auto-initializes on construction
 * and works correctly in both heap and zero-heap modes.
 */

/* ── Mutex ──────────────────────────────────────────────────────────── */

static void test_cpp_static_mutex_init(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_static_mutex_lock_unlock(void **state)
{
	(void)state;
	ove::Mutex mtx;
	mtx.lock();
	mtx.unlock();
}

/* ── RecursiveMutex ─────────────────────────────────────────────────── */

static void test_cpp_static_recursive_mutex_init(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	assert_true(mtx.valid());
}

/* ── Semaphore ──────────────────────────────────────────────────────── */

static void test_cpp_static_semaphore_init(void **state)
{
	(void)state;
	ove::Semaphore sem(1, 1);
	assert_true(sem.valid());
	assert_true(sem.try_acquire());
	sem.release();
}

/* ── Event ──────────────────────────────────────────────────────────── */

static void test_cpp_static_event_init(void **state)
{
	(void)state;
	ove::Event evt;
	assert_true(evt.valid());
	evt.signal();
	assert_true(evt.try_wait());
}

/* ── CondVar ────────────────────────────────────────────────────────── */

static void test_cpp_static_condvar_init(void **state)
{
	(void)state;
	ove::CondVar cv;
	assert_true(cv.valid());
}

/* ── EventGroup ─────────────────────────────────────────────────────── */

static void test_cpp_static_eventgroup_init(void **state)
{
	(void)state;
	ove::EventGroup eg;
	assert_true(eg.valid());
	(void)eg.set_bits(0x01);
	assert_true(eg.get_bits() & 0x01);
}

/* ── Queue ──────────────────────────────────────────────────────────── */

static void test_cpp_static_queue_init(void **state)
{
	(void)state;
	ove::Queue<uint32_t, 4> q;
	assert_true(q.valid());

	uint32_t val = 42;
	assert_true(q.try_send(val));
	uint32_t out = 0;
	assert_true(q.try_receive(out));
	assert_int_equal(out, 42);
}

/* ── Timer ──────────────────────────────────────────────────────────── */

static std::atomic<int> g_timer_fired;

[[maybe_unused]] static void timer_static_cb(ove_timer_t, void *)
{
	g_timer_fired.store(1);
}

#ifndef __SANITIZE_THREAD__
static void test_cpp_static_timer_init(void **state)
{
	(void)state;
	g_timer_fired.store(0);
	ove::Timer tmr(timer_static_cb, nullptr, 50, true);
	assert_true(tmr.valid());
	assert_true(tmr.start().has_value());
	test_msleep(200);
	assert_true(g_timer_fired.load());
}
#endif

/* ── Thread ─────────────────────────────────────────────────────────── */

static std::atomic<int> g_static_thread_ran;

extern "C" {
static void static_thread_entry(void *arg)
{
	(void)arg;
	g_static_thread_ran.store(1);
}
} /* extern "C" */

static void test_cpp_static_thread_init(void **state)
{
	(void)state;
	g_static_thread_ran.store(0);

	ove::Thread<4096> t(static_thread_entry, nullptr, OVE_PRIO_NORMAL, "static_t");
	assert_true(t.valid());
	test_msleep(100);
	assert_true(g_static_thread_ran.load());
}

/* ── Stream ─────────────────────────────────────────────────────────── */

static void test_cpp_static_stream_init(void **state)
{
	(void)state;
	ove::Stream<64> s(1);
	assert_true(s.valid());

	const uint8_t data[] = {1, 2, 3};
	size_t sent = 0;
	assert_true(s.try_send(data, sizeof(data), sent));
	assert_int_equal(sent, sizeof(data));
}

/* ── Watchdog ───────────────────────────────────────────────────────── */

static void test_cpp_static_watchdog_init(void **state)
{
	(void)state;
	ove::Watchdog wdt(5000);
	assert_true(wdt.valid());
}

/* ── Workqueue ──────────────────────────────────────────────────────── */

static void test_cpp_static_workqueue_init(void **state)
{
	(void)state;
	ove::Workqueue<4096> wq("test_wq", OVE_PRIO_NORMAL);
	assert_true(wq.valid());
}

/* ── RAII cleanup on scope exit ─────────────────────────────────────── */

static void test_cpp_static_raii_deinit(void **state)
{
	(void)state;
	/* Verify that a constructed object is properly cleaned up
	 * by the destructor. */
	{
		ove::Mutex mtx;
		mtx.lock();
		mtx.unlock();
		/* destructor runs here */
	}
	/* If we get here without crash, RAII cleanup works */
}

/* ── Suite runner ───────────────────────────────────────────────────── */

int test_cpp_static_init_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_static_mutex_init),
		cmocka_unit_test(test_cpp_static_mutex_lock_unlock),
		cmocka_unit_test(test_cpp_static_recursive_mutex_init),
		cmocka_unit_test(test_cpp_static_semaphore_init),
		cmocka_unit_test(test_cpp_static_event_init),
		cmocka_unit_test(test_cpp_static_condvar_init),
		cmocka_unit_test(test_cpp_static_eventgroup_init),
		cmocka_unit_test(test_cpp_static_queue_init),
#ifndef __SANITIZE_THREAD__
		cmocka_unit_test(test_cpp_static_timer_init),
#endif
		cmocka_unit_test(test_cpp_static_thread_init),
		cmocka_unit_test(test_cpp_static_stream_init),
		cmocka_unit_test(test_cpp_static_watchdog_init),
		cmocka_unit_test(test_cpp_static_workqueue_init),
		cmocka_unit_test(test_cpp_static_raii_deinit),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
