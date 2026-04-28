#include "../framework/ove_test.hpp"

struct cpp_rmtx_ctx {
	ove::RecursiveMutex *mtx;
	volatile int locked;
};

extern "C" {

static void cpp_rmtx_hold_entry(void *arg)
{
	auto *ctx = static_cast<cpp_rmtx_ctx *>(arg);
	(void)ctx->mtx->lock(OVE_WAIT_FOREVER);
	ctx->locked = 1;
	test_msleep(200);
	ctx->mtx->unlock();
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_recursive_create(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	assert_true(mtx.valid());
}

static void test_cpp_recursive_lock_twice(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	assert_int_equal(mtx.lock(OVE_WAIT_FOREVER), OVE_OK);
	assert_int_equal(mtx.lock(OVE_WAIT_FOREVER), OVE_OK);
	mtx.unlock();
	mtx.unlock();
}

static void test_cpp_recursive_matching_unlocks(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	for (int i = 0; i < 3; i++)
		(void)mtx.lock(OVE_WAIT_FOREVER);
	for (int i = 0; i < 3; i++)
		mtx.unlock();
	assert_int_equal(mtx.lock(0), OVE_OK);
	mtx.unlock();
}

static void test_cpp_recursive_timeout(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	cpp_rmtx_ctx ctx = {&mtx, 0};
	auto th = make_test_thread("rh", cpp_rmtx_hold_entry, &ctx);
	while (!ctx.locked)
		test_msleep(1);
	assert_int_equal(mtx.lock(50), OVE_ERR_TIMEOUT);
}

static void test_cpp_recursive_destroy(void **state)
{
	(void)state;
	ove::RecursiveMutex mtx;
	/* RAII destroy */
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_recursive_raii_destroy(void **state)
{
	(void)state;
	{
		ove::RecursiveMutex mtx;
		(void)mtx.lock(OVE_WAIT_FOREVER);
		mtx.unlock();
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_recursive_move_construct(void **state)
{
	(void)state;
	ove::RecursiveMutex a;
	assert_true(a.valid());

	ove::RecursiveMutex b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}

static void test_cpp_recursive_move_assign(void **state)
{
	(void)state;
	ove::RecursiveMutex a, b;

	b = std::move(a);
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_recursive_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::RecursiveMutex>::value,
		      "RecursiveMutex must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::RecursiveMutex>::value,
		      "RecursiveMutex must not be copy assignable");
}

int test_cpp_recursive_mutex_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_recursive_create),
		cmocka_unit_test(test_cpp_recursive_lock_twice),
		cmocka_unit_test(test_cpp_recursive_matching_unlocks),
		cmocka_unit_test(test_cpp_recursive_timeout),
		cmocka_unit_test(test_cpp_recursive_destroy),
		cmocka_unit_test(test_cpp_recursive_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_recursive_move_construct),
		cmocka_unit_test(test_cpp_recursive_move_assign),
#endif
		cmocka_unit_test(test_cpp_recursive_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
