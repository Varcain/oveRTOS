#include "../framework/ove_test.hpp"

static void test_audio_process(int16_t *out, const int16_t *in,
			       unsigned int frame_count, void *user_data)
{
	(void)out;
	(void)in;
	(void)frame_count;
	(void)user_data;
}

static void test_cpp_audio_init_deinit(void **state)
{
	(void)state;
	struct ove_audio_config cfg = {};
	cfg.sample_rate = 48000;
	cfg.channels = 2;
	cfg.bit_depth = 16;
	cfg.frames_per_buffer = 256;

	int ret = ove::audio::init(&cfg, test_audio_process, nullptr);
	assert_int_equal(ret, OVE_OK);

	ove::audio::deinit();
}

static void test_cpp_audio_start_stop(void **state)
{
	(void)state;
	struct ove_audio_config cfg = {};
	cfg.sample_rate = 48000;
	cfg.channels = 2;
	cfg.bit_depth = 16;
	cfg.frames_per_buffer = 256;

	(void)ove::audio::init(&cfg, test_audio_process, nullptr);

	int ret = ove::audio::start();
	assert_int_equal(ret, OVE_OK);

	ret = ove::audio::stop();
	assert_int_equal(ret, OVE_OK);

	ove::audio::deinit();
}

static void test_cpp_audio_pause_resume(void **state)
{
	(void)state;
	struct ove_audio_config cfg = {};
	cfg.sample_rate = 48000;
	cfg.channels = 2;
	cfg.bit_depth = 16;
	cfg.frames_per_buffer = 256;

	(void)ove::audio::init(&cfg, test_audio_process, nullptr);
	(void)ove::audio::start();

	int ret = ove::audio::pause();
	assert_int_equal(ret, OVE_OK);

	ret = ove::audio::resume();
	assert_int_equal(ret, OVE_OK);

	(void)ove::audio::stop();
	ove::audio::deinit();
}

int test_cpp_audio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_audio_init_deinit),
		cmocka_unit_test(test_cpp_audio_start_stop),
		cmocka_unit_test(test_cpp_audio_pause_resume),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
