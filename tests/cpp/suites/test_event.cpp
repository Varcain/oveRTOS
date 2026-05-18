#include "../framework/ove_test.hpp"

struct cpp_evt_ctx {
	ove::Event *evt;
	volatile int done;
};

extern "C" {

static void cpp_evt_signal_entry(void *arg)
{
	auto *ctx = static_cast<cpp_evt_ctx *>(arg);
	test_msleep(50);
	ctx->evt->signal();
	ctx->done = 1;
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_event_create(void **state)
{
	(void)state;
	ove::Event evt;
	assert_true(evt.valid());
}

static void test_cpp_event_destroy_basic(void **state)
{
	(void)state;
	ove::Event evt;
}

static void test_cpp_event_signal_then_wait(void **state)
{
	(void)state;
	ove::Event evt;
	evt.signal();
	assert_true(evt.try_wait());
}

static void test_cpp_event_wait_timeout(void **state)
{
	(void)state;
	ove::Event evt;
	ove::Result<void> r = evt.try_wait_for(std::chrono::milliseconds{50});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

static void test_cpp_event_cross_thread(void **state)
{
	(void)state;
	ove::Event evt;
	cpp_evt_ctx ctx = {&evt, 0};

	{
		auto th = make_test_thread("esig", cpp_evt_signal_entry, &ctx);
		ove::Result<void> r = evt.try_wait_for(std::chrono::milliseconds{500});
		assert_true(r.has_value());
	}
	assert_int_equal(ctx.done, 1);
}

static void test_cpp_event_signal_from_isr(void **state)
{
	(void)state;
	ove::Event evt;
	evt.signal_from_isr();
	assert_true(evt.try_wait());
}

static void test_cpp_event_auto_reset(void **state)
{
	(void)state;
	ove::Event evt;
	evt.signal();
	assert_true(evt.try_wait());
	ove::Result<void> r = evt.try_wait_for(std::chrono::milliseconds{50});
	assert_false(r.has_value());
	assert_true(r.error() == ove::Error::Timeout);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_event_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Event evt;
		evt.signal();
		(void)evt.try_wait();
	}
}

#ifndef CONFIG_OVE_ZERO_HEAP
/* Move is deleted in zero-heap mode (wrapper owns inline storage). */
static void test_cpp_event_move_construct(void **state)
{
	(void)state;
	ove::Event a;
	assert_true(a.valid());

	ove::Event b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}
#endif /* !CONFIG_OVE_ZERO_HEAP */

static void test_cpp_event_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Event>::value,
		      "Event must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Event>::value,
		      "Event must not be copy assignable");
}

/* Method-return-type pins.  Catches an accidental revert of the
 * `try_wait_for`/`try_wait_until` migration from `int` to
 * `Result<void>` at compile time. */
static void test_cpp_event_return_type_shape(void **state)
{
	(void)state;
	static_assert(std::is_same_v<decltype(std::declval<ove::Event>().wait()), void>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Event>().try_wait()), bool>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Event>().try_wait_for(
					     std::chrono::milliseconds{1})),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Event>().try_wait_until(
					     std::chrono::steady_clock::now())),
				     ove::Result<void>>);
	static_assert(std::is_same_v<decltype(std::declval<ove::Event>().signal()), void>);
}

int test_cpp_event_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_event_create),
		cmocka_unit_test(test_cpp_event_destroy_basic),
		cmocka_unit_test(test_cpp_event_signal_then_wait),
		cmocka_unit_test(test_cpp_event_wait_timeout),
		cmocka_unit_test(test_cpp_event_cross_thread),
		cmocka_unit_test(test_cpp_event_signal_from_isr),
		cmocka_unit_test(test_cpp_event_auto_reset),
		cmocka_unit_test(test_cpp_event_raii_destroy),
#ifndef CONFIG_OVE_ZERO_HEAP
		cmocka_unit_test(test_cpp_event_move_construct),
#endif
		cmocka_unit_test(test_cpp_event_not_copyable),
		cmocka_unit_test(test_cpp_event_return_type_shape),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
