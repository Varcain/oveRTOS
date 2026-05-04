#include "../framework/ove_test.hpp"

struct cpp_counter_ctx {
	ove::Mutex *mtx;
	int counter;
};

extern "C" {

static void cpp_counter_entry(void *arg)
{
	auto *ctx = static_cast<cpp_counter_ctx *>(arg);
	for (int i = 0; i < 1000; i++) {
		(void)ctx->mtx->lock();
		ctx->counter++;
		ctx->mtx->unlock();
	}
}

struct cpp_hold_ctx {
	ove::Mutex *mtx;
	volatile int locked;
	volatile int released;
	int hold_ms;
};

static void cpp_hold_entry(void *arg)
{
	auto *ctx = static_cast<cpp_hold_ctx *>(arg);
	(void)ctx->mtx->lock();
	__atomic_store_n(&ctx->locked, 1, __ATOMIC_RELEASE);
	test_msleep(ctx->hold_ms);
	ctx->mtx->unlock();
	__atomic_store_n(&ctx->released, 1, __ATOMIC_RELEASE);
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_mutex_create(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_mutex_destroy_safe(void **state)
{
	(void)state;
	ove::Mutex mtx;
	/* RAII: destructor cleans up */
}

static void test_cpp_mutex_lock_unlock(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_int_equal(mtx.lock(OVE_WAIT_FOREVER), OVE_OK);
	mtx.unlock();
}

static void test_cpp_mutex_contention_timeout(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 200};
	auto th = make_test_thread("hold", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	assert_int_equal(mtx.lock(50), OVE_ERR_TIMEOUT);
}

static void test_cpp_mutex_contention_success(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 50};
	auto th = make_test_thread("rel", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	assert_int_equal(mtx.lock(500), OVE_OK);
	mtx.unlock();
}

static void test_cpp_mutex_double_unlock(void **state)
{
	(void)state;
#ifdef __SANITIZE_THREAD__
	skip(); /* TSan flags double-unlock as UB; same rationale as C side. */
#else
	ove::Mutex mtx;
	(void)mtx.lock(OVE_WAIT_FOREVER);
	mtx.unlock();
	mtx.unlock(); /* should not crash */
#endif
}

static void test_cpp_mutex_zero_timeout_free(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_int_equal(mtx.lock(0), OVE_OK);
	mtx.unlock();
}

static void test_cpp_mutex_shared_counter(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_counter_ctx ctx = {&mtx, 0};
	{
		auto t1 = make_test_thread("c1", cpp_counter_entry, &ctx);
		auto t2 = make_test_thread("c2", cpp_counter_entry, &ctx);
	}

	assert_int_equal(ctx.counter, 2000);
}

static void test_cpp_mutex_short_timeout(void **state)
{
	(void)state;
	ove::Mutex mtx;

	cpp_hold_ctx ctx = {&mtx, 0, 0, 200};
	auto th = make_test_thread("h2", cpp_hold_entry, &ctx);
	for (int i = 0; i < 500 && !__atomic_load_n(&ctx.locked, __ATOMIC_ACQUIRE); i++)
		test_msleep(1);

	uint64_t start = 0, end = 0;
	ove_time_get_us(&start);
	int rc = mtx.lock(50);
	ove_time_get_us(&end);

	assert_int_equal(rc, OVE_ERR_TIMEOUT);
	assert_duration_within(end - start, 50, OVE_TEST_TIMING_TOLERANCE_MS);
}

static void test_cpp_mutex_multiple_independent(void **state)
{
	(void)state;
	ove::Mutex a, b;
	assert_int_equal(a.lock(OVE_WAIT_FOREVER), OVE_OK);
	assert_int_equal(b.lock(OVE_WAIT_FOREVER), OVE_OK);
	b.unlock();
	a.unlock();
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_mutex_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Mutex mtx;
		(void)mtx.lock(OVE_WAIT_FOREVER);
		mtx.unlock();
		/* mtx goes out of scope — destructor cleans up */
	}
	/* If we get here without crash, RAII destroy works */
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move semantics are only supported in heap-allocating mode: in zero-heap
 * builds the wrapper owns inline storage and Move is deleted to prevent
 * dangling-handle bugs. */
static void test_cpp_mutex_move_construct(void **state)
{
	(void)state;
	ove::Mutex a;
	assert_true(a.valid());

	ove::Mutex b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}

static void test_cpp_mutex_move_assign(void **state)
{
	(void)state;
	ove::Mutex a, b;

	b = std::move(a);
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_mutex_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Mutex>::value,
		      "Mutex must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Mutex>::value,
		      "Mutex must not be copy assignable");
}

static void test_cpp_mutex_valid_after_construct(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_mutex_handle_access(void **state)
{
	(void)state;
	ove::Mutex mtx;
	assert_true(mtx.handle() != nullptr);
}

int test_cpp_mutex_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_mutex_create),
		cmocka_unit_test(test_cpp_mutex_destroy_safe),
		cmocka_unit_test(test_cpp_mutex_lock_unlock),
		cmocka_unit_test(test_cpp_mutex_contention_timeout),
		cmocka_unit_test(test_cpp_mutex_contention_success),
		cmocka_unit_test(test_cpp_mutex_double_unlock),
		cmocka_unit_test(test_cpp_mutex_zero_timeout_free),
		cmocka_unit_test(test_cpp_mutex_shared_counter),
		cmocka_unit_test(test_cpp_mutex_short_timeout),
		cmocka_unit_test(test_cpp_mutex_multiple_independent),
		cmocka_unit_test(test_cpp_mutex_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_mutex_move_construct),
		cmocka_unit_test(test_cpp_mutex_move_assign),
#endif
		cmocka_unit_test(test_cpp_mutex_not_copyable),
		cmocka_unit_test(test_cpp_mutex_valid_after_construct),
		cmocka_unit_test(test_cpp_mutex_handle_access),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
