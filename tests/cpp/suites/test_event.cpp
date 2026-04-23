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
	assert_int_equal(evt.wait(0), OVE_OK);
}

static void test_cpp_event_wait_timeout(void **state)
{
	(void)state;
	ove::Event evt;
	assert_int_equal(evt.wait(50), OVE_ERR_TIMEOUT);
}

static void test_cpp_event_cross_thread(void **state)
{
	(void)state;
	ove::Event evt;
	cpp_evt_ctx ctx = {&evt, 0};

	{
		auto th = make_test_thread("esig", cpp_evt_signal_entry, &ctx);
		assert_int_equal(evt.wait(500), OVE_OK);
	}
	assert_int_equal(ctx.done, 1);
}

static void test_cpp_event_signal_from_isr(void **state)
{
	(void)state;
	ove::Event evt;
	evt.signal_from_isr();
	assert_int_equal(evt.wait(0), OVE_OK);
}

static void test_cpp_event_auto_reset(void **state)
{
	(void)state;
	ove::Event evt;
	evt.signal();
	assert_int_equal(evt.wait(0), OVE_OK);
	assert_int_equal(evt.wait(50), OVE_ERR_TIMEOUT);
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_event_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Event evt;
		evt.signal();
		(void)evt.wait(0);
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
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
