#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "ove/types.h"
#include "ove/audio.h"

/* ── Mock node ops ──────────────────────────────────────────────── */

static int mock_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                 struct ove_audio_fmt *out)
{
    (void)ctx;
    (void)in;
    out->sample_rate = 48000;
    out->channels = 1;
    out->sample_fmt = OVE_AUDIO_FMT_S16;
    return OVE_OK;
}

static int mock_proc_configure(void *ctx, const struct ove_audio_fmt *in,
                               struct ove_audio_fmt *out)
{
    (void)ctx;
    *out = *in; /* passthrough format */
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

static const struct ove_audio_node_ops mock_source_ops = {
    .configure = mock_source_configure,
    .process   = mock_process,
};

static const struct ove_audio_node_ops mock_proc_ops = {
    .configure = mock_proc_configure,
    .process   = mock_process,
};

static const struct ove_audio_node_ops mock_sink_ops = {
    .configure = mock_sink_configure,
    .process   = mock_process,
};

/* ── Graph construction tests ───────────────────────────────────── */

static void test_graph_init(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    assert_int_equal(ove_audio_graph_init(&g, 512), OVE_OK);
    assert_int_equal(g.frames_per_period, 512);
    assert_int_equal(g.node_count, 0);
    assert_int_equal(g.edge_count, 0);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_IDLE);
    ove_audio_graph_deinit(&g);
}

static void test_graph_init_null(void **state)
{
    (void)state;
    assert_int_equal(ove_audio_graph_init(NULL, 512), OVE_ERR_INVALID_PARAM);
}

static void test_graph_init_zero_frames(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    assert_int_equal(ove_audio_graph_init(&g, 0), OVE_ERR_INVALID_PARAM);
}

static void test_graph_add_node(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int idx = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                       OVE_AUDIO_NODE_SOURCE);
    assert_int_equal(idx, 0);
    assert_int_equal(g.node_count, 1);

    idx = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "proc",
                                   OVE_AUDIO_NODE_PROCESSOR);
    assert_int_equal(idx, 1);

    ove_audio_graph_deinit(&g);
}

static void test_graph_connect(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int src  = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                        OVE_AUDIO_NODE_SOURCE);
    int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "sink",
                                        OVE_AUDIO_NODE_SINK);

    assert_int_equal(ove_audio_graph_connect(&g, src, sink), OVE_OK);
    assert_int_equal(g.edge_count, 1);

    ove_audio_graph_deinit(&g);
}

static void test_graph_connect_sink_output_rejected(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "sink",
                                        OVE_AUDIO_NODE_SINK);
    int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "proc",
                                        OVE_AUDIO_NODE_PROCESSOR);

    assert_int_equal(ove_audio_graph_connect(&g, sink, proc),
                     OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

static void test_graph_connect_source_input_rejected(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "proc",
                                        OVE_AUDIO_NODE_PROCESSOR);
    int src  = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                        OVE_AUDIO_NODE_SOURCE);

    assert_int_equal(ove_audio_graph_connect(&g, proc, src),
                     OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

/* ── Build tests ────────────────────────────────────────────────── */

static void test_graph_build_simple(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int src  = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                        OVE_AUDIO_NODE_SOURCE);
    int proc = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "proc",
                                        OVE_AUDIO_NODE_PROCESSOR);
    int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "sink",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, proc);
    ove_audio_graph_connect(&g, proc, sink);

    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

    assert_int_equal(g.exec_count, 3);
    assert_int_equal(g.exec_order[0], (unsigned int)src);
    assert_int_equal(g.exec_order[1], (unsigned int)proc);
    assert_int_equal(g.exec_order[2], (unsigned int)sink);

    assert_int_equal(g.nodes[src].out_fmt.sample_rate, 48000);
    assert_int_equal(g.nodes[src].out_fmt.channels, 1);
    assert_int_equal(g.nodes[src].out_fmt.sample_fmt, OVE_AUDIO_FMT_S16);

    ove_audio_graph_deinit(&g);
}

static void test_graph_build_cycle_rejected(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int a = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "a",
                                     OVE_AUDIO_NODE_PROCESSOR);
    int b = ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "b",
                                     OVE_AUDIO_NODE_PROCESSOR);

    int src = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                       OVE_AUDIO_NODE_SOURCE);
    ove_audio_graph_connect(&g, src, a);
    ove_audio_graph_connect(&g, a, b);
    /* b→a is rejected by fan-in guard (a already has incoming edge from src) */
    assert_int_equal(ove_audio_graph_connect(&g, b, a), OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

static int mismatched_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                                     struct ove_audio_fmt *out)
{
    (void)ctx;
    (void)out;
    if (in->sample_rate != 96000)
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static const struct ove_audio_node_ops mismatched_sink_ops = {
    .configure = mismatched_sink_configure,
    .process   = mock_process,
};

static void test_graph_build_format_mismatch(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 256);

    int src  = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                        OVE_AUDIO_NODE_SOURCE);
    int sink = ove_audio_graph_add_node(&g, &mismatched_sink_ops, NULL, "sink",
                                        OVE_AUDIO_NODE_SINK);
    ove_audio_graph_connect(&g, src, sink);

    assert_int_equal(ove_audio_graph_build(&g), OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

/* ── Data-flow mock nodes ───────────────────────────────────────── */

/* Source that fills output buffer with incrementing int16 values */
static int counting_source_process(void *ctx, const struct ove_audio_buf *in,
                                   struct ove_audio_buf *out)
{
    (void)ctx;
    (void)in;
    int16_t *samples = (int16_t *)out->data;
    for (unsigned int i = 0; i < out->frames; i++)
        samples[i] = (int16_t)i;
    return OVE_OK;
}

static const struct ove_audio_node_ops counting_source_ops = {
    .configure = mock_source_configure,
    .process   = counting_source_process,
};

/* Processor that doubles each sample */
static int doubler_process(void *ctx, const struct ove_audio_buf *in,
                           struct ove_audio_buf *out)
{
    (void)ctx;
    const int16_t *src = (const int16_t *)in->data;
    int16_t *dst = (int16_t *)out->data;
    for (unsigned int i = 0; i < in->frames; i++)
        dst[i] = src[i] * 2;
    return OVE_OK;
}

static const struct ove_audio_node_ops doubler_ops = {
    .configure = mock_proc_configure,
    .process   = doubler_process,
};

/* Sink that copies received data to a test buffer for verification */
struct verify_sink_ctx {
    int16_t *captured;
    unsigned int captured_frames;
};

static int verify_sink_process(void *ctx, const struct ove_audio_buf *in,
                               struct ove_audio_buf *out)
{
    (void)out;
    struct verify_sink_ctx *vs = (struct verify_sink_ctx *)ctx;
    memcpy(vs->captured, in->data, in->frames * sizeof(int16_t));
    vs->captured_frames = in->frames;
    return OVE_OK;
}

static const struct ove_audio_node_ops verify_sink_ops = {
    .configure = mock_sink_configure,
    .process   = verify_sink_process,
};

/* ── Data flow test ─────────────────────────────────────────────── */

static void test_graph_data_flow(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 8); /* small buffer for testing */

    int src  = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                        OVE_AUDIO_NODE_SOURCE);
    int dbl  = ove_audio_graph_add_node(&g, &doubler_ops, NULL, "double",
                                        OVE_AUDIO_NODE_PROCESSOR);

    int16_t captured[8] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, dbl);
    ove_audio_graph_connect(&g, dbl, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

    /* Pump one cycle */
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* Verify: source produces 0,1,2,...7; doubler makes 0,2,4,...14 */
    assert_int_equal(vs.captured_frames, 8);
    for (int i = 0; i < 8; i++)
        assert_int_equal(captured[i], i * 2);

    ove_audio_graph_deinit(&g);
}

/* ── Fan-out test ───────────────────────────────────────────────── */

static void test_graph_fan_out(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                       OVE_AUDIO_NODE_SOURCE);

    int16_t cap_a[4] = {0}, cap_b[4] = {0};
    struct verify_sink_ctx vs_a = { .captured = cap_a };
    struct verify_sink_ctx vs_b = { .captured = cap_b };

    int sink_a = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs_a, "sink-a",
                                           OVE_AUDIO_NODE_SINK);
    int sink_b = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs_b, "sink-b",
                                           OVE_AUDIO_NODE_SINK);

    /* Fan-out: source feeds both sinks */
    ove_audio_graph_connect(&g, src, sink_a);
    ove_audio_graph_connect(&g, src, sink_b);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* Both sinks should receive 0,1,2,3 */
    for (int i = 0; i < 4; i++) {
        assert_int_equal(cap_a[i], i);
        assert_int_equal(cap_b[i], i);
    }

    ove_audio_graph_deinit(&g);
}

/* ── Node error produces silence ────────────────────────────────── */

static int failing_process(void *ctx, const struct ove_audio_buf *in,
                           struct ove_audio_buf *out)
{
    (void)ctx; (void)in; (void)out;
    return OVE_ERR_ML_FAILED; /* arbitrary error */
}

static const struct ove_audio_node_ops failing_proc_ops = {
    .configure = mock_proc_configure,
    .process   = failing_process,
};

static void test_graph_node_error_silence(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 4);

    int src  = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                        OVE_AUDIO_NODE_SOURCE);
    int fail = ove_audio_graph_add_node(&g, &failing_proc_ops, NULL, "fail",
                                        OVE_AUDIO_NODE_PROCESSOR);

    int16_t captured[4] = {99, 99, 99, 99};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, fail);
    ove_audio_graph_connect(&g, fail, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* Failing processor output should be zeroed (silence) */
    for (int i = 0; i < 4; i++)
        assert_int_equal(captured[i], 0);

    /* Stats should record the error */
    struct ove_audio_graph_stats stats;
    ove_audio_graph_get_stats(&g, &stats);
    assert_int_equal(stats.node_errors, 1);
    assert_int_equal(stats.cycles, 1);

    ove_audio_graph_deinit(&g);
}

/* ── Converter tests ────────────────────────────────────────────── */

/* Source that outputs S16 samples with known values */
static int s16_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                struct ove_audio_fmt *out)
{
    (void)ctx; (void)in;
    out->sample_rate = 48000;
    out->channels = 1;
    out->sample_fmt = OVE_AUDIO_FMT_S16;
    return OVE_OK;
}

static int s16_source_process(void *ctx, const struct ove_audio_buf *in,
                              struct ove_audio_buf *out)
{
    (void)ctx; (void)in;
    int16_t *s = (int16_t *)out->data;
    s[0] = 1000;
    s[1] = -1000;
    s[2] = 16383;
    s[3] = -16383;
    return OVE_OK;
}

static const struct ove_audio_node_ops s16_source_ops = {
    .configure = s16_source_configure,
    .process   = s16_source_process,
};

/* Sink that captures float data */
struct f32_sink_ctx {
    float *captured;
    unsigned int count;
};

static int f32_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                              struct ove_audio_fmt *out)
{
    (void)ctx; (void)out;
    if (in->sample_fmt != OVE_AUDIO_FMT_F32)
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static int f32_sink_process(void *ctx, const struct ove_audio_buf *in,
                            struct ove_audio_buf *out)
{
    (void)out;
    struct f32_sink_ctx *fs = (struct f32_sink_ctx *)ctx;
    memcpy(fs->captured, in->data, in->frames * sizeof(float));
    fs->count = in->frames;
    return OVE_OK;
}

static const struct ove_audio_node_ops f32_sink_ops = {
    .configure = f32_sink_configure,
    .process   = f32_sink_process,
};

static void test_converter_s16_to_f32(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &s16_source_ops, NULL, "s16-src",
                                       OVE_AUDIO_NODE_SOURCE);
    int conv = ove_audio_node_converter(&g, OVE_AUDIO_FMT_F32, "to-f32");

    float cap[4] = {0};
    struct f32_sink_ctx fs = { .captured = cap };
    int sink = ove_audio_graph_add_node(&g, &f32_sink_ops, &fs, "f32-sink",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, conv);
    ove_audio_graph_connect(&g, conv, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* 1000/32768 ≈ 0.0305 */
    assert_true(fabsf(cap[0] - 1000.0f / 32768.0f) < 0.001f);
    assert_true(fabsf(cap[1] - (-1000.0f / 32768.0f)) < 0.001f);

    ove_audio_graph_deinit(&g);
}

/* ── Channel mapper test ────────────────────────────────────────── */

/* Stereo source */
static int stereo_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                   struct ove_audio_fmt *out)
{
    (void)ctx; (void)in;
    out->sample_rate = 48000;
    out->channels = 2;
    out->sample_fmt = OVE_AUDIO_FMT_S16;
    return OVE_OK;
}

static int stereo_source_process(void *ctx, const struct ove_audio_buf *in,
                                 struct ove_audio_buf *out)
{
    (void)ctx; (void)in;
    int16_t *s = (int16_t *)out->data;
    /* Frame 0: L=100, R=200; Frame 1: L=300, R=400 */
    s[0] = 100; s[1] = 200;
    s[2] = 300; s[3] = 400;
    return OVE_OK;
}

static const struct ove_audio_node_ops stereo_source_ops = {
    .configure = stereo_source_configure,
    .process   = stereo_source_process,
};

/* Mono S16 verify sink */
static int mono_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                               struct ove_audio_fmt *out)
{
    (void)ctx; (void)out;
    if (in->channels != 1)
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static const struct ove_audio_node_ops mono_verify_sink_ops = {
    .configure = mono_sink_configure,
    .process   = verify_sink_process,
};

static void test_channel_map_stereo_to_mono(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 2); /* 2 frames */

    int src = ove_audio_graph_add_node(&g, &stereo_source_ops, NULL, "stereo",
                                       OVE_AUDIO_NODE_SOURCE);

    /* Extract right channel (index 1) to mono */
    struct ove_audio_channel_map map = {
        .out_channels = 1,
        .map = { 1 }, /* out ch0 = in ch1 (right) */
    };
    int mapper = ove_audio_node_channel_map(&g, &map, "r-to-mono");

    int16_t captured[2] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &mono_verify_sink_ops, &vs,
                                        "mono-sink", OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, mapper);
    ove_audio_graph_connect(&g, mapper, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* Should get right channel: 200, 400 */
    assert_int_equal(captured[0], 200);
    assert_int_equal(captured[1], 400);

    ove_audio_graph_deinit(&g);
}

/* ── Gain test ──────────────────────────────────────────────────── */

static void test_gain_node(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                       OVE_AUDIO_NODE_SOURCE);
    int gain = ove_audio_node_gain(&g, -6.0f, "gain"); /* ~0.5x */

    int16_t captured[4] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, gain);
    ove_audio_graph_connect(&g, gain, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* -6dB ≈ 0.501x. Source produces 0,1,2,3. */
    assert_int_equal(captured[0], 0);
    assert_true(captured[2] <= 2);
    assert_true(captured[3] <= 2);

    ove_audio_graph_deinit(&g);
}

/* ── Tap test ───────────────────────────────────────────────────── */

static _Atomic unsigned int tap_call_count;
static int16_t tap_last_sample;

static void test_tap_callback(const struct ove_audio_buf *buf, void *user_data)
{
    (void)user_data;
    const int16_t *s = (const int16_t *)buf->data;
    tap_last_sample = s[0];
    tap_call_count++;
}

static void test_tap_node(void **state)
{
    (void)state;
    struct ove_audio_graph g;
    ove_audio_graph_init(&g, 4);

    tap_call_count = 0;
    tap_last_sample = 0;

    int src = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                       OVE_AUDIO_NODE_SOURCE);
    int tap = ove_audio_node_tap(&g, test_tap_callback, NULL, "tap");

    int16_t captured[4] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, tap);
    ove_audio_graph_connect(&g, tap, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    assert_int_equal(tap_call_count, 1);
    assert_int_equal(tap_last_sample, 0);

    for (int i = 0; i < 4; i++)
        assert_int_equal(captured[i], i);

    ove_audio_graph_deinit(&g);
}

int test_audio_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_graph_init),
        cmocka_unit_test(test_graph_init_null),
        cmocka_unit_test(test_graph_init_zero_frames),
        cmocka_unit_test(test_graph_add_node),
        cmocka_unit_test(test_graph_connect),
        cmocka_unit_test(test_graph_connect_sink_output_rejected),
        cmocka_unit_test(test_graph_connect_source_input_rejected),
        cmocka_unit_test(test_graph_build_simple),
        cmocka_unit_test(test_graph_build_cycle_rejected),
        cmocka_unit_test(test_graph_build_format_mismatch),
        cmocka_unit_test(test_graph_data_flow),
        cmocka_unit_test(test_graph_fan_out),
        cmocka_unit_test(test_graph_node_error_silence),
        cmocka_unit_test(test_converter_s16_to_f32),
        cmocka_unit_test(test_channel_map_stereo_to_mono),
        cmocka_unit_test(test_gain_node),
        cmocka_unit_test(test_tap_node),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
