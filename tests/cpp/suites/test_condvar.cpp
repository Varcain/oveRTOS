#include "../framework/ove_test.hpp"

struct cpp_cv_waiter_ctx {
	ove::CondVar *cv;
	ove::Mutex *mtx;
	volatile int woke;
};

struct cpp_cv_signal_ctx {
	ove::CondVar *cv;
	ove::Mutex *mtx;
	volatile int signaled;
};

struct cpp_cv_prod_ctx {
	ove::CondVar *cv;
	ove::Mutex *mtx;
	volatile int ready;
};

extern "C" {

static void cpp_cv_wait_entry(void *arg)
{
	auto *ctx = static_cast<cpp_cv_waiter_ctx *>(arg);
	ctx->mtx->lock();
	ctx->cv->wait(*ctx->mtx);
	ctx->woke = 1;
	ctx->mtx->unlock();
}

static void cpp_cv_signal_entry(void *arg)
{
	auto *ctx = static_cast<cpp_cv_signal_ctx *>(arg);
	test_msleep(50);
	ctx->mtx->lock();
	ctx->signaled = 1;
	ctx->cv->notify_one();
	ctx->mtx->unlock();
}

static void cpp_cv_producer_entry(void *arg)
{
	auto *ctx = static_cast<cpp_cv_prod_ctx *>(arg);
	test_msleep(50);
	ctx->mtx->lock();
	ctx->ready = 1;
	ctx->cv->notify_one();
	ctx->mtx->unlock();
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_condvar_create(void **state)
{
	(void)state;
	ove::CondVar cv;
	assert_true(cv.valid());
}

static void test_cpp_condvar_destroy_basic(void **state)
{
	(void)state;
	ove::CondVar cv;
}

static void test_cpp_condvar_signal_wakes_one(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;

	cpp_cv_waiter_ctx ctx = {&cv, &mtx, 0};
	{
		auto th = make_test_thread("cvw", cpp_cv_wait_entry, &ctx);
		test_msleep(50);

		mtx.lock();
		cv.notify_one();
		mtx.unlock();
	}
	assert_int_equal(ctx.woke, 1);
}

static void test_cpp_condvar_broadcast(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;

	cpp_cv_waiter_ctx c1 = {&cv, &mtx, 0};
	cpp_cv_waiter_ctx c2 = {&cv, &mtx, 0};

	{
		auto t1 = make_test_thread("w1", cpp_cv_wait_entry, &c1);
		auto t2 = make_test_thread("w2", cpp_cv_wait_entry, &c2);
		test_msleep(100);

		mtx.lock();
		cv.notify_all();
		mtx.unlock();
	}
	assert_int_equal(c1.woke, 1);
	assert_int_equal(c2.woke, 1);
}

static void test_cpp_condvar_wait_timeout(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;
	mtx.lock();
	ove::Result<void> r = cv.try_wait_for(mtx, std::chrono::milliseconds{50});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
	mtx.unlock();
}

static void test_cpp_condvar_producer_consumer(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;

	cpp_cv_prod_ctx ctx = {&cv, &mtx, 0};
	{
		auto th = make_test_thread("prod", cpp_cv_producer_entry, &ctx);

		mtx.lock();
		while (!ctx.ready)
			cv.wait(mtx);
		mtx.unlock();
	}
	assert_int_equal(ctx.ready, 1);
}

static void test_cpp_condvar_wait_forever(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;

	cpp_cv_signal_ctx ctx = {&cv, &mtx, 0};
	{
		auto th = make_test_thread("sig", cpp_cv_signal_entry, &ctx);

		mtx.lock();
		cv.wait(mtx);
		mtx.unlock();
	}
	assert_int_equal(ctx.signaled, 1);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_condvar_raii_destroy(void **state)
{
	(void)state;
	{
		ove::CondVar cv;
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_condvar_move_construct(void **state)
{
	(void)state;
	ove::CondVar a;
	assert_true(a.valid());

	ove::CondVar b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_condvar_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::CondVar>::value,
		      "CondVar must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::CondVar>::value,
		      "CondVar must not be copy assignable");
}

/* Method-return-type pins.  Basic try_wait_for/until return
 * Result<void> (consistent with Mutex/Semaphore/Event); predicate
 * overloads return bool (caller wants the pred-satisfied truth value,
 * not a Result-shape wrapping it). */
static void test_cpp_condvar_return_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<
		decltype(std::declval<ove::CondVar>().wait(std::declval<ove::Mutex &>())),
		void>);
	static_assert(std::is_same_v<decltype(std::declval<ove::CondVar>().try_wait_for(
					     std::declval<ove::Mutex &>(),
					     std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::CondVar>().try_wait_until(
					     std::declval<ove::Mutex &>(),
					     std::chrono::steady_clock::now())),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::CondVar>().try_wait_for(
					     std::declval<ove::Mutex &>(),
					     std::chrono::milliseconds{1}, [] { return true; })),
				     bool>);
	static_assert(std::is_same_v<decltype(std::declval<ove::CondVar>().try_wait_until(
					     std::declval<ove::Mutex &>(),
					     std::chrono::steady_clock::now(),
					     [] { return true; })),
				     bool>);
}

/* ── Iter A3: predicate-overload wait_*  — spurious-wakeup safety ──── */

/* 1. Predicate already true on entry: returns immediately. */
static void test_cpp_condvar_wait_predicate_already_true(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;
	bool ready = true;

	mtx.lock();
	const bool ok = cv.try_wait_for(mtx, std::chrono::milliseconds{500},
					 [&] { return ready; });
	mtx.unlock();
	assert_true(ok);
}

/* 2. Predicate becomes true via notify: returns true. */
static void test_cpp_condvar_wait_predicate_becomes_true(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;

	cpp_cv_prod_ctx ctx{&cv, &mtx, 0};
	auto th = make_test_thread("prod", cpp_cv_producer_entry, &ctx);

	mtx.lock();
	const bool ok = cv.try_wait_for(mtx, std::chrono::milliseconds{1000},
					 [&] { return ctx.ready != 0; });
	mtx.unlock();
	assert_true(ok);
	assert_int_equal(ctx.ready, 1);
}

/* 3. Predicate stays false past timeout: returns false. */
static void test_cpp_condvar_wait_predicate_timeout(void **state)
{
	(void)state;
	ove::CondVar cv;
	ove::Mutex mtx;
	bool never_ready = false;

	mtx.lock();
	const auto t0 = ove::steady_clock::now();
	const bool ok = cv.try_wait_for(mtx, std::chrono::milliseconds{50},
					 [&] { return never_ready; });
	const auto elapsed = ove::steady_clock::now() - t0;
	mtx.unlock();

	assert_false(ok);
	const auto elapsed_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	assert_duration_within(static_cast<uint64_t>(elapsed_ms * 1000), 50000,
			       OVE_TEST_TIMING_TOLERANCE_MS * 1000);
}

int test_cpp_condvar_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_condvar_create),
		cmocka_unit_test(test_cpp_condvar_destroy_basic),
		cmocka_unit_test(test_cpp_condvar_signal_wakes_one),
		cmocka_unit_test(test_cpp_condvar_broadcast),
		cmocka_unit_test(test_cpp_condvar_wait_timeout),
		cmocka_unit_test(test_cpp_condvar_producer_consumer),
		cmocka_unit_test(test_cpp_condvar_wait_forever),
		cmocka_unit_test(test_cpp_condvar_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_condvar_move_construct),
#endif
		cmocka_unit_test(test_cpp_condvar_not_copyable),
		cmocka_unit_test(test_cpp_condvar_return_type_shape),
		cmocka_unit_test(test_cpp_condvar_wait_predicate_already_true),
		cmocka_unit_test(test_cpp_condvar_wait_predicate_becomes_true),
		cmocka_unit_test(test_cpp_condvar_wait_predicate_timeout),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
