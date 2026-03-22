#include "../framework/ove_test.hpp"

#define BIT_0 (1u << 0)
#define BIT_1 (1u << 1)
#define BIT_2 (1u << 2)

struct cpp_setter_ctx {
	ove::EventGroup *eg;
	uint32_t bits_to_set;
	uint32_t delay_ms;
};

extern "C" {

static void cpp_setter_thread(void *arg)
{
	auto *ctx = static_cast<cpp_setter_ctx *>(arg);
	test_msleep(ctx->delay_ms);
	ctx->eg->set_bits(ctx->bits_to_set);
}

} /* extern "C" */

/* ── Mirrored tests ─────────────────────────────────────────────────── */

static void test_cpp_eg_create_destroy(void **state)
{
	(void)state;
	ove::EventGroup eg;
	assert_true(eg.valid());
}

static void test_cpp_eg_set_bits(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits(BIT_0 | BIT_1);
	ove_eventbits_t bits = eg.get_bits();
	assert_true(bits & BIT_0);
	assert_true(bits & BIT_1);
}

static void test_cpp_eg_clear_bits(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits(BIT_0 | BIT_1 | BIT_2);
	eg.clear_bits(BIT_1);
	ove_eventbits_t remaining = eg.get_bits();
	assert_true(remaining & BIT_0);
	assert_false(remaining & BIT_1);
	assert_true(remaining & BIT_2);
}

static void test_cpp_eg_get_bits(void **state)
{
	(void)state;
	ove::EventGroup eg;

	assert_int_equal(eg.get_bits(), 0);
	eg.set_bits(BIT_2);
	assert_true(eg.get_bits() & BIT_2);
}

static void test_cpp_eg_wait_all(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits(BIT_0 | BIT_1);

	ove_eventbits_t actual = 0;
	int rc = eg.wait_bits(BIT_0 | BIT_1, OVE_EG_WAIT_ALL, 100, &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true((actual & (BIT_0 | BIT_1)) == (BIT_0 | BIT_1));
}

static void test_cpp_eg_wait_any(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits(BIT_0);

	ove_eventbits_t actual = 0;
	int rc = eg.wait_bits(BIT_0 | BIT_1, 0, 100, &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true(actual & BIT_0);
}

static void test_cpp_eg_wait_timeout(void **state)
{
	(void)state;
	ove::EventGroup eg;

	ove_eventbits_t actual = 0;
	int rc = eg.wait_bits(BIT_0, OVE_EG_WAIT_ALL, 10, &actual);
	assert_int_equal(rc, OVE_ERR_TIMEOUT);
}

static void test_cpp_eg_clear_on_exit(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits(BIT_0 | BIT_1);

	ove_eventbits_t actual = 0;
	int rc = eg.wait_bits(BIT_0 | BIT_1,
			      OVE_EG_WAIT_ALL | OVE_EG_CLEAR_ON_EXIT,
			      100, &actual);
	assert_int_equal(rc, OVE_OK);

	ove_eventbits_t remaining = eg.get_bits();
	assert_false(remaining & BIT_0);
	assert_false(remaining & BIT_1);
}

static void test_cpp_eg_set_bits_from_isr(void **state)
{
	(void)state;
	ove::EventGroup eg;

	eg.set_bits_from_isr(BIT_2);
	assert_true(eg.get_bits() & BIT_2);
}

static void test_cpp_eg_cross_thread(void **state)
{
	(void)state;
	ove::EventGroup eg;

	cpp_setter_ctx ctx = {&eg, BIT_0 | BIT_1, 50};
	auto th = make_test_thread("setter", cpp_setter_thread, &ctx, OVE_PRIO_LOW);

	ove_eventbits_t actual = 0;
	int rc = eg.wait_bits(BIT_0 | BIT_1, OVE_EG_WAIT_ALL, 500, &actual);
	assert_int_equal(rc, OVE_OK);
	assert_true((actual & (BIT_0 | BIT_1)) == (BIT_0 | BIT_1));
}

/* ── Wrapper-specific tests ─────────────────────────────────────────── */

static void test_cpp_eg_raii_destroy(void **state)
{
	(void)state;
	{
		ove::EventGroup eg;
		eg.set_bits(BIT_0);
	}
}

static void test_cpp_eg_move_construct(void **state)
{
	(void)state;
	ove::EventGroup a;
	assert_true(a.valid());

	ove::EventGroup b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}

static void test_cpp_eg_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::EventGroup>::value,
		      "EventGroup must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::EventGroup>::value,
		      "EventGroup must not be copy assignable");
}

int test_cpp_eventgroup_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_eg_create_destroy),
		cmocka_unit_test(test_cpp_eg_set_bits),
		cmocka_unit_test(test_cpp_eg_clear_bits),
		cmocka_unit_test(test_cpp_eg_get_bits),
		cmocka_unit_test(test_cpp_eg_wait_all),
		cmocka_unit_test(test_cpp_eg_wait_any),
		cmocka_unit_test(test_cpp_eg_wait_timeout),
		cmocka_unit_test(test_cpp_eg_clear_on_exit),
		cmocka_unit_test(test_cpp_eg_set_bits_from_isr),
		cmocka_unit_test(test_cpp_eg_cross_thread),
		cmocka_unit_test(test_cpp_eg_raii_destroy),
		cmocka_unit_test(test_cpp_eg_move_construct),
		cmocka_unit_test(test_cpp_eg_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
