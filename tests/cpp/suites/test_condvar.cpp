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
	assert_int_equal(cv.try_wait_for(mtx, std::chrono::milliseconds{50}), OVE_ERR_TIMEOUT);
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
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
