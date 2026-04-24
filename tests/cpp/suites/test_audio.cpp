#include "../framework/ove_test.hpp"
#include "ove/audio.h"
#include "ove/audio_node.h"

#include <string.h>

/* Graph struct ~900 bytes — use static to avoid stack overflow on embedded */
static struct ove_audio_graph g;
#include <math.h>

/*
 * Zero-heap build cannot calloc inter-node buffers; callers must supply
 * storage via ove_audio_graph_set_buf_storage() before build().  Every
 * test here shares one conservatively sized static buffer — wrapping
 * init + set_buf_storage in graph_init() keeps each test body free of
 * the #ifdef.
 */
#ifdef CONFIG_OVE_ZERO_HEAP
static uint8_t audio_graph_test_buf[OVE_AUDIO_GRAPH_STORAGE_BYTES(
    OVE_AUDIO_GRAPH_MAX_NODES, 256, 2, 4)] __attribute__((aligned(4)));
#endif

static int graph_init(struct ove_audio_graph *g, unsigned int frames)
{
	int r = ove_audio_graph_init(g, frames);
#ifdef CONFIG_OVE_ZERO_HEAP
	if (r == OVE_OK)
		r = ove_audio_graph_set_buf_storage(g, audio_graph_test_buf,
		                                    sizeof(audio_graph_test_buf));
#endif
	return r;
}

static void test_cpp_audio_graph_init_deinit(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));

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
	memset(&g, 0, sizeof(g));

	int ret = ove_audio_graph_init(&g, 0);
	assert_int_not_equal(ret, OVE_OK);
}

/* ── Mock node ops (C linkage for vtable compatibility) ─────────── */

static int mock_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                 struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)in;
	out->sample_rate = 48000;
	out->channels    = 1;
	out->sample_fmt  = OVE_AUDIO_FMT_S16;
	return OVE_OK;
}

static int mock_proc_configure(void *ctx, const struct ove_audio_fmt *in,
                               struct ove_audio_fmt *out)
{
	(void)ctx;
	*out = *in;
	return OVE_OK;
}

static int mock_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                               struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)in;
	(void)out;
	return OVE_OK;
}

static int mock_process(void *ctx, const struct ove_audio_buf *in,
                        struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	(void)out;
	return OVE_OK;
}

/* Field order: configure, start, stop, process, destroy */
static const struct ove_audio_node_ops mock_source_ops = {
	mock_source_configure, nullptr, nullptr, mock_process, nullptr,
};
static const struct ove_audio_node_ops mock_proc_ops = {
	mock_proc_configure, nullptr, nullptr, mock_process, nullptr,
};
static const struct ove_audio_node_ops mock_sink_ops = {
	mock_sink_configure, nullptr, nullptr, mock_process, nullptr,
};

/* Source that fills output with incrementing int16 values */
static int counting_source_process(void *ctx, const struct ove_audio_buf *in,
                                   struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	int16_t *samples = static_cast<int16_t *>(out->data);
	for (unsigned int i = 0; i < out->frames; i++)
		samples[i] = static_cast<int16_t>(i);
	return OVE_OK;
}

static const struct ove_audio_node_ops counting_source_ops = {
	mock_source_configure, nullptr, nullptr, counting_source_process, nullptr,
};

/* Sink that captures received data for verification */
struct verify_sink_ctx {
	int16_t     *captured;
	unsigned int captured_frames;
};

static int verify_sink_process(void *ctx, const struct ove_audio_buf *in,
                               struct ove_audio_buf *out)
{
	(void)out;
	auto *vs = static_cast<struct verify_sink_ctx *>(ctx);
	memcpy(vs->captured, in->data, in->frames * sizeof(int16_t));
	vs->captured_frames = in->frames;
	return OVE_OK;
}

static const struct ove_audio_node_ops verify_sink_ops = {
	mock_sink_configure, nullptr, nullptr, verify_sink_process, nullptr,
};

/* The ove_audio_node_{converter,channel_map,gain,tap} factories internally
 * malloc their per-node context and are therefore gated behind OVE_HEAP_AUDIO.
 * Under CONFIG_OVE_ZERO_HEAP the gate is undefined, so the helpers and tests
 * below compile out entirely. */
#ifdef OVE_HEAP_AUDIO

/* Source that outputs known S16 values for converter test */
static int s16_source_process(void *ctx, const struct ove_audio_buf *in,
                              struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	auto *s = static_cast<int16_t *>(out->data);
	s[0] = 1000;
	s[1] = -1000;
	s[2] = 16383;
	s[3] = -16383;
	return OVE_OK;
}

static const struct ove_audio_node_ops s16_source_ops = {
	mock_source_configure, nullptr, nullptr, s16_source_process, nullptr,
};

/* F32 sink that captures float data */
struct f32_sink_ctx {
	float       *captured;
	unsigned int count;
};

static int f32_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                              struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)out;
	if (in->sample_fmt != OVE_AUDIO_FMT_F32)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static int f32_sink_process(void *ctx, const struct ove_audio_buf *in,
                            struct ove_audio_buf *out)
{
	(void)out;
	auto *fs = static_cast<struct f32_sink_ctx *>(ctx);
	memcpy(fs->captured, in->data, in->frames * sizeof(float));
	fs->count = in->frames;
	return OVE_OK;
}

static const struct ove_audio_node_ops f32_sink_ops = {
	f32_sink_configure, nullptr, nullptr, f32_sink_process, nullptr,
};

/* Tap callback state */
static unsigned int cpp_tap_call_count;
static int16_t      cpp_tap_last_sample;

static void cpp_tap_callback(const struct ove_audio_buf *buf, void *user_data)
{
	(void)user_data;
	auto *s = static_cast<const int16_t *>(buf->data);
	cpp_tap_last_sample = s[0];
	cpp_tap_call_count++;
}

#endif /* OVE_HEAP_AUDIO */

/* ── Graph API tests ───────────────────────────────────────────── */

static void test_cpp_audio_add_node(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 256);

	int src = ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                                   OVE_AUDIO_NODE_SOURCE);
	assert_int_equal(src, 0);

	int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, nullptr, "proc",
	                                    OVE_AUDIO_NODE_PROCESSOR);
	assert_int_equal(proc, 1);

	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);
	assert_int_equal(sink, 2);

	assert_int_equal(g.node_count, 3u);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_connect(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 256);

	int src  = ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                                    OVE_AUDIO_NODE_SOURCE);
	int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, nullptr, "proc",
	                                    OVE_AUDIO_NODE_PROCESSOR);
	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);

	assert_int_equal(ove_audio_graph_connect(&g, src, proc), OVE_OK);
	assert_int_equal(g.edge_count, 1u);

	assert_int_equal(ove_audio_graph_connect(&g, proc, sink), OVE_OK);
	assert_int_equal(g.edge_count, 2u);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_build_simple(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 256);

	int src  = ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                                    OVE_AUDIO_NODE_SOURCE);
	int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, nullptr, "proc",
	                                    OVE_AUDIO_NODE_PROCESSOR);
	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, proc);
	ove_audio_graph_connect(&g, proc, sink);

	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

	/* Topological order: src, proc, sink */
	assert_int_equal(g.exec_count, 3u);
	assert_int_equal(g.exec_order[0], static_cast<unsigned int>(src));
	assert_int_equal(g.exec_order[1], static_cast<unsigned int>(proc));
	assert_int_equal(g.exec_order[2], static_cast<unsigned int>(sink));

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_start_stop(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 256);

	ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                         OVE_AUDIO_NODE_SOURCE);
	int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, nullptr, "proc",
	                                    OVE_AUDIO_NODE_PROCESSOR);
	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, 0, proc);
	ove_audio_graph_connect(&g, proc, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

	assert_int_equal(ove_audio_graph_start(&g), OVE_OK);
	assert_int_equal(g.state, OVE_AUDIO_GRAPH_RUNNING);

	assert_int_equal(ove_audio_graph_stop(&g), OVE_OK);
	assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_process(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 8);

	int src = ove_audio_graph_add_node(&g, &counting_source_ops, nullptr,
	                                   "count", OVE_AUDIO_NODE_SOURCE);

	int16_t captured[8] = {0};
	struct verify_sink_ctx vs = { captured, 0 };
	int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	/* Source produces 0,1,2,...,7; sink should capture them directly */
	assert_int_equal(vs.captured_frames, 8u);
	for (int i = 0; i < 8; i++)
		assert_int_equal(captured[i], static_cast<int16_t>(i));

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_stats(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 4);

	int src  = ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                                    OVE_AUDIO_NODE_SOURCE);
	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

	/* Process 3 cycles */
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	struct ove_audio_graph_stats stats;
	assert_int_equal(ove_audio_graph_get_stats(&g, &stats), OVE_OK);
	assert_int_equal(stats.cycles, 3u);
	assert_int_equal(stats.node_errors, 0u);

	ove_audio_graph_deinit(&g);
}

#ifdef OVE_HEAP_AUDIO

static void test_cpp_audio_converter_node(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 4);

	int src = ove_audio_graph_add_node(&g, &s16_source_ops, nullptr,
	                                   "s16-src", OVE_AUDIO_NODE_SOURCE);
	int conv = ove_audio_node_converter(&g, OVE_AUDIO_FMT_F32, "to-f32");
	assert_true(conv >= 0);

	float cap[4] = {0};
	struct f32_sink_ctx fs = { cap, 0 };
	int sink = ove_audio_graph_add_node(&g, &f32_sink_ops, &fs, "f32-sink",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, conv);
	ove_audio_graph_connect(&g, conv, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	/* S16 1000 -> F32: 1000/32768 ~ 0.0305 */
	assert_true(fabsf(cap[0] - 1000.0f / 32768.0f) < 0.001f);
	assert_true(fabsf(cap[1] - (-1000.0f / 32768.0f)) < 0.001f);
	/* S16 16383 -> F32: 16383/32768 ~ 0.5 */
	assert_true(fabsf(cap[2] - 16383.0f / 32768.0f) < 0.001f);
	assert_true(fabsf(cap[3] - (-16383.0f / 32768.0f)) < 0.001f);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_gain_node(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 4);

	int src = ove_audio_graph_add_node(&g, &counting_source_ops, nullptr,
	                                   "count", OVE_AUDIO_NODE_SOURCE);
	int gain = ove_audio_node_gain(&g, -6.0f, "gain");
	assert_true(gain >= 0);

	int16_t captured[4] = {0};
	struct verify_sink_ctx vs = { captured, 0 };
	int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, gain);
	ove_audio_graph_connect(&g, gain, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	/* -6 dB ~ 0.501x.  Source produces 0,1,2,3. */
	assert_int_equal(captured[0], 0); /* 0 * 0.5 = 0 */
	/* Samples 2 and 3 should be attenuated roughly by half */
	assert_true(captured[2] <= 2);
	assert_true(captured[3] <= 2);
	/* They should still be positive (source values were positive) */
	assert_true(captured[2] >= 0);
	assert_true(captured[3] >= 0);

	ove_audio_graph_deinit(&g);
}

static void test_cpp_audio_tap_node(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 4);

	cpp_tap_call_count  = 0;
	cpp_tap_last_sample = 0;

	int src = ove_audio_graph_add_node(&g, &counting_source_ops, nullptr,
	                                   "count", OVE_AUDIO_NODE_SOURCE);
	int tap = ove_audio_node_tap(&g, cpp_tap_callback, nullptr, "tap");
	assert_true(tap >= 0);

	int16_t captured[4] = {0};
	struct verify_sink_ctx vs = { captured, 0 };
	int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
	                                    OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, tap);
	ove_audio_graph_connect(&g, tap, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	/* Tap callback should have fired exactly once */
	assert_int_equal(cpp_tap_call_count, 1u);
	/* First sample from counting source is 0 */
	assert_int_equal(cpp_tap_last_sample, 0);

	/* Data should pass through the tap unmodified */
	for (int i = 0; i < 4; i++)
		assert_int_equal(captured[i], static_cast<int16_t>(i));

	ove_audio_graph_deinit(&g);
}

/* ── Channel-map test helpers ───────────────────────────────────── */

static int stereo_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                   struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)in;
	out->sample_rate = 48000;
	out->channels    = 2;
	out->sample_fmt  = OVE_AUDIO_FMT_S16;
	return OVE_OK;
}

static int stereo_source_process(void *ctx, const struct ove_audio_buf *in,
                                 struct ove_audio_buf *out)
{
	(void)ctx;
	(void)in;
	int16_t *s = static_cast<int16_t *>(out->data);
	/* Frame 0: L=100, R=200; Frame 1: L=300, R=400 */
	s[0] = 100; s[1] = 200;
	s[2] = 300; s[3] = 400;
	return OVE_OK;
}

static const struct ove_audio_node_ops stereo_source_ops = {
	stereo_source_configure, nullptr, nullptr, stereo_source_process, nullptr,
};

static int mono_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                               struct ove_audio_fmt *out)
{
	(void)ctx;
	(void)out;
	if (in->channels != 1)
		return OVE_ERR_INVALID_PARAM;
	return OVE_OK;
}

static const struct ove_audio_node_ops mono_verify_sink_ops = {
	mono_sink_configure, nullptr, nullptr, verify_sink_process, nullptr,
};

static void test_cpp_audio_channel_map(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 2); /* 2 frames */

	int src = ove_audio_graph_add_node(&g, &stereo_source_ops, nullptr,
	                                   "stereo", OVE_AUDIO_NODE_SOURCE);

	/* Extract left channel (index 0) to mono */
	struct ove_audio_channel_map map;
	memset(&map, 0, sizeof(map));
	map.out_channels = 1;
	map.map[0] = 0; /* out ch0 = in ch0 (left) */

	int mapper = ove_audio_node_channel_map(&g, &map, "l-to-mono");
	assert_true(mapper >= 0);

	int16_t captured[2] = {0};
	struct verify_sink_ctx vs;
	memset(&vs, 0, sizeof(vs));
	vs.captured = captured;

	int sink = ove_audio_graph_add_node(&g, &mono_verify_sink_ops, &vs,
	                                    "mono-sink", OVE_AUDIO_NODE_SINK);

	ove_audio_graph_connect(&g, src, mapper);
	ove_audio_graph_connect(&g, mapper, sink);
	assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
	assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

	/* Should get left channel: 100, 300 */
	assert_int_equal(captured[0], 100);
	assert_int_equal(captured[1], 300);

	ove_audio_graph_deinit(&g);
}

#endif /* OVE_HEAP_AUDIO */

static void test_cpp_audio_connect_invalid(void **state)
{
	(void)state;
	memset(&g, 0, sizeof(g));
	graph_init(&g, 256);

	int src  = ove_audio_graph_add_node(&g, &mock_source_ops, nullptr, "src",
	                                    OVE_AUDIO_NODE_SOURCE);
	int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, nullptr, "proc",
	                                    OVE_AUDIO_NODE_PROCESSOR);
	int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, nullptr, "sink",
	                                    OVE_AUDIO_NODE_SINK);

	/* Self-loop: from == to */
	assert_int_not_equal(ove_audio_graph_connect(&g, proc, proc), OVE_OK);

	/* Out-of-bounds index */
	assert_int_not_equal(ove_audio_graph_connect(&g, src, 99), OVE_OK);
	assert_int_not_equal(ove_audio_graph_connect(&g, 99, proc), OVE_OK);

	/* Sink as upstream (producer) */
	assert_int_not_equal(ove_audio_graph_connect(&g, sink, proc), OVE_OK);

	/* Source as downstream (consumer) */
	assert_int_not_equal(ove_audio_graph_connect(&g, proc, src), OVE_OK);

	ove_audio_graph_deinit(&g);
}

int test_cpp_audio_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_cpp_audio_graph_init_deinit),
		cmocka_unit_test(test_cpp_audio_graph_init_null),
		cmocka_unit_test(test_cpp_audio_graph_init_zero_frames),
		cmocka_unit_test(test_cpp_audio_add_node),
		cmocka_unit_test(test_cpp_audio_connect),
		cmocka_unit_test(test_cpp_audio_build_simple),
		cmocka_unit_test(test_cpp_audio_start_stop),
		cmocka_unit_test(test_cpp_audio_process),
		cmocka_unit_test(test_cpp_audio_stats),
#ifdef OVE_HEAP_AUDIO
		cmocka_unit_test(test_cpp_audio_converter_node),
		cmocka_unit_test(test_cpp_audio_gain_node),
		cmocka_unit_test(test_cpp_audio_tap_node),
		cmocka_unit_test(test_cpp_audio_channel_map),
#endif
		cmocka_unit_test(test_cpp_audio_connect_invalid),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
