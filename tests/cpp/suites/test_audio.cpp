#include "../framework/ove_test.hpp"
#include "ove/audio.h"

static void test_cpp_audio_graph_init_deinit(void **state)
{
	(void)state;
	struct ove_audio_graph g;

	int ret = ove_audio_graph_init(&g, 256);
	assert_int_equal(ret, OVE_OK);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_graph_init_null(void **state)
{
	(void)state;
	int ret = ove_audio_graph_init(nullptr, 256);
	assert_int_not_equal(ret, OVE_OK);
}

static void test_cpp_audio_graph_init_zero_frames(void **state)
{
	(void)state;
	struct ove_audio_graph g;

	int ret = ove_audio_graph_init(&g, 0);
	assert_int_not_equal(ret, OVE_OK);
}

int test_cpp_audio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_audio_graph_init_deinit),
		cmocka_unit_test(test_cpp_audio_graph_init_null),
		cmocka_unit_test(test_cpp_audio_graph_init_zero_frames),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
