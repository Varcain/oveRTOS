#include "../framework/ove_test.hpp"
#include <cstring>

static void test_cpp_stream_create_destroy(void **state)
{
	(void)state;
	ove::Stream<256> s(1);
	assert_true(s.valid());
}

static void test_cpp_stream_send_receive(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t tx[] = { 0xDE, 0xAD, 0xBE, 0xEF };
	size_t sent = 0;
	int ret = s.send(tx, sizeof(tx), 100, &sent);
	assert_int_equal(ret, OVE_OK);
	assert_int_equal(sent, sizeof(tx));

	uint8_t rx[4] = {};
	size_t received = 0;
	ret = s.receive(rx, sizeof(rx), 100, &received);
	assert_int_equal(ret, OVE_OK);
	assert_int_equal(received, sizeof(tx));
	assert_memory_equal(rx, tx, sizeof(tx));
}

static void test_cpp_stream_bytes_available(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	assert_int_equal(s.bytes_available(), 0);

	const uint8_t data[] = { 1, 2, 3 };
	size_t sent = 0;
	(void)s.send(data, sizeof(data), 100, &sent);
	assert_true(s.bytes_available() >= 3);
}

static void test_cpp_stream_reset(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t data[] = { 1, 2, 3 };
	size_t sent = 0;
	(void)s.send(data, sizeof(data), 100, &sent);
	assert_true(s.bytes_available() > 0);

	assert_int_equal(s.reset(), OVE_OK);
	assert_int_equal(s.bytes_available(), 0);
}

static void test_cpp_stream_send_from_isr(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t data[] = { 0xAA, 0xBB };
	size_t sent = 0;
	int ret = s.send_from_isr(data, sizeof(data), &sent);
	assert_int_equal(ret, OVE_OK);
	assert_int_equal(sent, 2);
}

static void test_cpp_stream_receive_from_isr(void **state)
{
	(void)state;
	ove::Stream<256> s(1);

	const uint8_t tx[] = { 0x11, 0x22 };
	size_t sent = 0;
	(void)s.send(tx, sizeof(tx), 100, &sent);

	uint8_t rx[2] = {};
	size_t received = 0;
	int ret = s.receive_from_isr(rx, sizeof(rx), &received);
	assert_int_equal(ret, OVE_OK);
	assert_int_equal(received, 2);
	assert_memory_equal(rx, tx, 2);
}

static void test_cpp_stream_raii_destroy(void **state)
{
	(void)state;
	{
		ove::Stream<128> s(1);
		const uint8_t d[] = { 1 };
		size_t n = 0;
		(void)s.send(d, 1, 0, &n);
	}
}

static void test_cpp_stream_move_construct(void **state)
{
	(void)state;
	ove::Stream<128> a(1);
	assert_true(a.valid());

	ove::Stream<128> b(std::move(a));
	assert_true(b.valid());
	assert_false(a.valid());
}

static void test_cpp_stream_not_copyable(void **state)
{
	(void)state;
	static_assert(!std::is_copy_constructible<ove::Stream<64>>::value,
		      "Stream must not be copy constructible");
	static_assert(!std::is_copy_assignable<ove::Stream<64>>::value,
		      "Stream must not be copy assignable");
}

int test_cpp_stream_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_stream_create_destroy),
		cmocka_unit_test(test_cpp_stream_send_receive),
		cmocka_unit_test(test_cpp_stream_bytes_available),
		cmocka_unit_test(test_cpp_stream_reset),
		cmocka_unit_test(test_cpp_stream_send_from_isr),
		cmocka_unit_test(test_cpp_stream_receive_from_isr),
		cmocka_unit_test(test_cpp_stream_raii_destroy),
		cmocka_unit_test(test_cpp_stream_move_construct),
		cmocka_unit_test(test_cpp_stream_not_copyable),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
