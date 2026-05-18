#include "../framework/ove_test.hpp"

struct cpp_sem_ctx {
	ove::Semaphore *sem;
	volatile int done;
};

extern "C" {

static void cpp_sem_give_entry(void *arg)
{
	auto *ctx = static_cast<cpp_sem_ctx *>(arg);
	test_msleep(50);
	ctx->sem->release();
	ctx->done = 1;
}

static void cpp_sem_give_delayed_entry(void *arg)
{
	auto *ctx = static_cast<cpp_sem_ctx *>(arg);
	test_msleep(100);
	ctx->sem->release();
	ctx->done = 1;
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_sem_create_binary(void **state)
{
	(void)state;
	ove::Semaphore sem(1, 1);
	assert_true(sem.valid());
}

static void test_cpp_sem_create_counting(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
}

static void test_cpp_sem_take_initial_one(void **state)
{
	(void)state;
	ove::Semaphore sem(1, 1);
	assert_true(sem.try_acquire());
}

static void test_cpp_sem_take_timeout(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	ove::Result<void> r = sem.try_acquire_for(std::chrono::milliseconds{50});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

static void test_cpp_sem_give_then_take(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	sem.release();
	assert_true(sem.try_acquire());
}

static void test_cpp_sem_counting(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 10);
	for (int i = 0; i < 3; i++)
		sem.release();
	for (int i = 0; i < 3; i++)
		assert_true(sem.try_acquire());
	ove::Result<void> r = sem.try_acquire_for(std::chrono::milliseconds{10});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

static void test_cpp_sem_producer_consumer(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 1);
	cpp_sem_ctx ctx = {&sem, 0};

	{
		auto th = make_test_thread("prod", cpp_sem_give_entry, &ctx);
		ove::Result<void> r = sem.try_acquire_for(std::chrono::milliseconds{500});
		assert_true(r.has_value());
	}
	assert_int_equal(ctx.done, 1);
}

static void test_cpp_sem_wait_forever(void **state)
{
	(void)state;
	ove::Semaphore sem(0, 1);
	cpp_sem_ctx ctx = {&sem, 0};

	auto th = make_test_thread("wf", cpp_sem_give_delayed_entry, &ctx);
	sem.acquire();
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_sem_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Semaphore sem(1, 1);
		(void)sem.try_acquire();
		sem.release();
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_sem_move_construct(void **state)
{
	(void)state;
	ove::Semaphore a(1, 1);
	assert_true(a.valid());

	ove::Semaphore b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_sem_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Semaphore>::value,
		      "Semaphore must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Semaphore>::value,
		      "Semaphore must not be copy assignable");
}

/* Method-return-type pins.  Catches an accidental revert of the
 * `try_acquire_for`/`try_acquire_until` migration from `int` to
 * `Result<void>` at compile time. */
static void test_cpp_sem_return_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<decltype(std::declval<ove::Semaphore>().acquire()), void>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Semaphore>().try_acquire()), bool>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Semaphore>().try_acquire_for(
					     std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Semaphore>().try_acquire_until(
					     std::chrono::steady_clock::now())),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Semaphore>().release()), void>);
}

int test_cpp_semaphore_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_sem_create_binary),
		cmocka_unit_test(test_cpp_sem_create_counting),
		cmocka_unit_test(test_cpp_sem_take_initial_one),
		cmocka_unit_test(test_cpp_sem_take_timeout),
		cmocka_unit_test(test_cpp_sem_give_then_take),
		cmocka_unit_test(test_cpp_sem_counting),
		cmocka_unit_test(test_cpp_sem_producer_consumer),
		cmocka_unit_test(test_cpp_sem_wait_forever),
		cmocka_unit_test(test_cpp_sem_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_sem_move_construct),
#endif
		cmocka_unit_test(test_cpp_sem_not_copyable),
		cmocka_unit_test(test_cpp_sem_return_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
