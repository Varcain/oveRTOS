#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "ove/types.h"
#include "ove/audio.h"
#include "ove/audio_node.h"
#include "../framework/ove_test.h"

#ifdef CONFIG_OVE_AUDIO

/*
 * Graph struct is ~900 bytes — too large for embedded task stacks.
 * Use a single static instance shared across all tests.
 * Each test re-initialises it via ove_audio_graph_init().
 */
static struct ove_audio_graph g;

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

/* ── Mock start/stop ops for state-machine tests ────────────────── */

static int start_call_order[8];
static int stop_call_order[8];
static int start_call_count;
static int stop_call_count;
static int fail_on_start_idx = -1;

static int mock_start(void *ctx)
{
    int idx = ctx ? *(int *)ctx : 0;
    start_call_order[start_call_count++] = idx;
    if (idx == fail_on_start_idx)
        return OVE_ERR_ML_FAILED;
    return OVE_OK;
}

static int mock_stop(void *ctx)
{
    int idx = ctx ? *(int *)ctx : 0;
    stop_call_order[stop_call_count++] = idx;
    return OVE_OK;
}

static const struct ove_audio_node_ops mock_startable_source_ops = {
    .configure = mock_source_configure,
    .process   = mock_process,
    .start     = mock_start,
    .stop      = mock_stop,
};

static const struct ove_audio_node_ops mock_startable_proc_ops = {
    .configure = mock_proc_configure,
    .process   = mock_process,
    .start     = mock_start,
    .stop      = mock_stop,
};

static const struct ove_audio_node_ops mock_startable_sink_ops = {
    .configure = mock_sink_configure,
    .process   = mock_process,
    .start     = mock_start,
    .stop      = mock_stop,
};

/* Helper: reset start/stop tracking counters */
static void reset_start_stop_counters(void)
{
    start_call_count = 0;
    stop_call_count  = 0;
    fail_on_start_idx = -1;
    memset(start_call_order, 0, sizeof(start_call_order));
    memset(stop_call_order, 0, sizeof(stop_call_order));
}

/* Helper: build a 3-node startable graph (source->proc->sink) */
static void build_startable_graph(struct ove_audio_graph *g,
                                  int *node_ids, int *ctxs)
{
    ove_audio_graph_init(g, 256);
    ctxs[0] = 0; ctxs[1] = 1; ctxs[2] = 2;
    node_ids[0] = ove_audio_graph_add_node(g, &mock_startable_source_ops,
                                           &ctxs[0], "src",
                                           OVE_AUDIO_NODE_SOURCE);
    node_ids[1] = ove_audio_graph_add_node(g, &mock_startable_proc_ops,
                                           &ctxs[1], "proc",
                                           OVE_AUDIO_NODE_PROCESSOR);
    node_ids[2] = ove_audio_graph_add_node(g, &mock_startable_sink_ops,
                                           &ctxs[2], "sink",
                                           OVE_AUDIO_NODE_SINK);
    ove_audio_graph_connect(g, node_ids[0], node_ids[1]);
    ove_audio_graph_connect(g, node_ids[1], node_ids[2]);
    ove_audio_graph_build(g);
}

/* ── Graph construction tests ───────────────────────────────────── */

static void test_graph_init(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
    assert_int_equal(ove_audio_graph_init(&g, 0), OVE_ERR_INVALID_PARAM);
}

static void test_graph_add_node(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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
    memset(&g, 0, sizeof(g));
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

/* ── High-precision gain data-flow test ─────────────────────────── */

/*
 * Source that emits a known high-amplitude S16 waveform so that the
 * gain node's output stays well above the 1-LSB integer truncation
 * floor, letting us verify the linear-gain math with float tolerance.
 */
static const int16_t dataflow_source_samples[8] = {
    8000, 16000, 24000, 32000, -8000, -16000, -24000, -32000,
};

static int dataflow_source_process(void *ctx, const struct ove_audio_buf *in,
                                   struct ove_audio_buf *out)
{
    (void)ctx;
    (void)in;
    int16_t *samples = (int16_t *)out->data;
    unsigned int n = out->frames < 8 ? out->frames : 8;
    for (unsigned int i = 0; i < n; i++)
        samples[i] = dataflow_source_samples[i];
    return OVE_OK;
}

static const struct ove_audio_node_ops dataflow_source_ops = {
    .configure = mock_source_configure,
    .process   = dataflow_source_process,
};

/*
 * Push a known 8-sample S16 waveform through source→gain(-6dB)→sink
 * and assert each output sample matches the linear-gain prediction
 * within one LSB. Complements test_gain_node, which only checks
 * small-integer inputs where integer truncation dominates.
 */
static void test_graph_gain_dataflow(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 8);

    int src  = ove_audio_graph_add_node(&g, &dataflow_source_ops, NULL,
                                        "src", OVE_AUDIO_NODE_SOURCE);
    int gain = ove_audio_node_gain(&g, -6.0f, "gain");

    int16_t captured[8] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs,
                                        "sink", OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, gain);
    ove_audio_graph_connect(&g, gain, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);
    assert_int_equal(vs.captured_frames, 8);

    /* Padé approx of 10^(-6/20) ≈ 0.50119. */
    const float expected_gain = 0.50119f;
    for (int i = 0; i < 8; i++) {
        float expected = (float)dataflow_source_samples[i] * expected_gain;
        assert_float_within(captured[i], expected, 1.5f);
    }

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
    memset(&g, 0, sizeof(g));
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

/* ── State machine tests ────────────────────────────────────────── */

static void test_graph_start_stop(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    int ids[3], ctxs[3];

    reset_start_stop_counters();
    build_startable_graph(&g, ids, ctxs);

    /* READY -> RUNNING */
    assert_int_equal(ove_audio_graph_start(&g), OVE_OK);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_RUNNING);
    assert_int_equal(start_call_count, 3);

    /* RUNNING -> READY */
    assert_int_equal(ove_audio_graph_stop(&g), OVE_OK);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);
    assert_int_equal(stop_call_count, 3);

    /* Can start again */
    reset_start_stop_counters();
    assert_int_equal(ove_audio_graph_start(&g), OVE_OK);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_RUNNING);

    ove_audio_graph_stop(&g);
    ove_audio_graph_deinit(&g);
}

static void test_graph_start_from_idle_fails(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    /* Graph is IDLE (not built), start should fail */
    assert_int_equal(ove_audio_graph_start(&g), OVE_ERR_NOT_SUPPORTED);

    ove_audio_graph_deinit(&g);
}

static void test_graph_stop_from_idle_fails(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    /* Graph is IDLE, stop should fail */
    assert_int_equal(ove_audio_graph_stop(&g), OVE_ERR_NOT_SUPPORTED);

    ove_audio_graph_deinit(&g);
}

static void test_graph_start_failure_rollback(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    int ids[3], ctxs[3];

    reset_start_stop_counters();
    fail_on_start_idx = 2; /* 3rd node (sink) will fail */
    build_startable_graph(&g, ids, ctxs);

    assert_int_not_equal(ove_audio_graph_start(&g), OVE_OK);

    /* Nodes 0 and 1 started successfully before node 2 failed.
     * Rollback should have called stop() on nodes 1 and 0. */
    assert_int_equal(stop_call_count, 2);
    assert_int_equal(stop_call_order[0], 1);
    assert_int_equal(stop_call_order[1], 0);

    /* Graph should NOT be in RUNNING state */
    assert_int_not_equal(g.state, OVE_AUDIO_GRAPH_RUNNING);

    fail_on_start_idx = -1;
    ove_audio_graph_deinit(&g);
}

static void test_graph_add_node_after_build_fails(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    int ids[3], ctxs[3];

    build_startable_graph(&g, ids, ctxs);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

    /* Adding a node after build should fail */
    int ret = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "extra",
                                       OVE_AUDIO_NODE_SOURCE);
    assert_true(ret < 0);

    ove_audio_graph_deinit(&g);
}

static void test_graph_connect_after_build_fails(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    int ids[3], ctxs[3];

    build_startable_graph(&g, ids, ctxs);
    assert_int_equal(g.state, OVE_AUDIO_GRAPH_READY);

    /* Connecting after build should fail */
    assert_int_equal(ove_audio_graph_connect(&g, ids[0], ids[2]),
                     OVE_ERR_NOT_SUPPORTED);

    ove_audio_graph_deinit(&g);
}

/* ── Capacity tests ─────────────────────────────────────────────── */

static void test_graph_max_nodes(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    /* Add exactly 16 source nodes */
    for (int i = 0; i < OVE_AUDIO_GRAPH_MAX_NODES; i++) {
        int idx = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                           OVE_AUDIO_NODE_SOURCE);
        assert_true(idx >= 0);
    }

    /* 17th node should fail */
    int overflow = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "extra",
                                            OVE_AUDIO_NODE_SOURCE);
    assert_int_equal(overflow, OVE_ERR_QUEUE_FULL);

    ove_audio_graph_deinit(&g);
}

static void test_graph_max_edges(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    /* MAX_NODES=16, MAX_EDGES=16. Use 1 source + 15 sinks = 16 nodes, 15 edges.
     * Then verify we got as many edges as node slots allow (15).
     * The edge limit (16) can't actually be reached because nodes run out first. */
    int src = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                       OVE_AUDIO_NODE_SOURCE);
    assert_true(src >= 0);

    int edges_created = 0;
    for (int i = 0; i < OVE_AUDIO_GRAPH_MAX_NODES - 1; i++) {
        int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "sink",
                                            OVE_AUDIO_NODE_SINK);
        if (sink < 0)
            break;
        int ret = ove_audio_graph_connect(&g, src, sink);
        assert_int_equal(ret, OVE_OK);
        edges_created++;
    }

    /* Should have created MAX_NODES-1 edges (15) */
    assert_int_equal(edges_created, OVE_AUDIO_GRAPH_MAX_NODES - 1);
    assert_int_equal(g.edge_count, (unsigned int)edges_created);

    /* Node table is full — can't add more nodes to create more edges */
    int overflow = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "xsink",
                                            OVE_AUDIO_NODE_SINK);
    assert_int_equal(overflow, OVE_ERR_QUEUE_FULL);

    ove_audio_graph_deinit(&g);
}

/* ── Connect validation tests ───────────────────────────────────── */

static void test_graph_connect_self_loop(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    ove_audio_graph_add_node(&g, &mock_proc_ops, NULL, "proc",
                             OVE_AUDIO_NODE_PROCESSOR);

    assert_int_equal(ove_audio_graph_connect(&g, 0, 0), OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

static void test_graph_connect_invalid_index(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 256);

    ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                             OVE_AUDIO_NODE_SOURCE);

    assert_int_equal(ove_audio_graph_connect(&g, 99, 0), OVE_ERR_INVALID_PARAM);

    ove_audio_graph_deinit(&g);
}

/* ── Stats tests ────────────────────────────────────────────────── */

static void test_graph_stats_cycle_count(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4);

    int src  = ove_audio_graph_add_node(&g, &mock_source_ops, NULL, "src",
                                        OVE_AUDIO_NODE_SOURCE);
    int sink = ove_audio_graph_add_node(&g, &mock_sink_ops, NULL, "sink",
                                        OVE_AUDIO_NODE_SINK);
    ove_audio_graph_connect(&g, src, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

    for (int i = 0; i < 5; i++)
        assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    struct ove_audio_graph_stats stats;
    assert_int_equal(ove_audio_graph_get_stats(&g, &stats), OVE_OK);
    assert_int_equal(stats.cycles, 5);

    ove_audio_graph_deinit(&g);
}

static void test_graph_stats_null_params(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    struct ove_audio_graph_stats stats;

    ove_audio_graph_init(&g, 256);

    assert_int_not_equal(ove_audio_graph_get_stats(NULL, &stats), OVE_OK);
    assert_int_not_equal(ove_audio_graph_get_stats(&g, NULL), OVE_OK);

    ove_audio_graph_deinit(&g);
}

/* ── Multiple process cycles ────────────────────────────────────── */

/* Sink that counts how many times process was called */
struct counting_sink_ctx {
    int process_count;
};

static int counting_sink_process(void *ctx, const struct ove_audio_buf *in,
                                 struct ove_audio_buf *out)
{
    (void)in;
    (void)out;
    struct counting_sink_ctx *cs = (struct counting_sink_ctx *)ctx;
    cs->process_count++;
    return OVE_OK;
}

static const struct ove_audio_node_ops counting_sink_ops = {
    .configure = mock_sink_configure,
    .process   = counting_sink_process,
};

static void test_graph_process_multiple_cycles(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "src",
                                       OVE_AUDIO_NODE_SOURCE);

    struct counting_sink_ctx cs = { .process_count = 0 };
    int sink = ove_audio_graph_add_node(&g, &counting_sink_ops, &cs, "sink",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);

    for (int i = 0; i < 10; i++)
        assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    assert_int_equal(cs.process_count, 10);

    ove_audio_graph_deinit(&g);
}

/* ── Format conversion: F32 to S16 ─────────────────────────────── */

/* Source that outputs F32 samples with known values */
static int f32_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                struct ove_audio_fmt *out)
{
    (void)ctx; (void)in;
    out->sample_rate = 48000;
    out->channels = 1;
    out->sample_fmt = OVE_AUDIO_FMT_F32;
    return OVE_OK;
}

static int f32_source_process(void *ctx, const struct ove_audio_buf *in,
                              struct ove_audio_buf *out)
{
    (void)ctx; (void)in;
    float *s = (float *)out->data;
    s[0] =  0.5f;
    s[1] = -0.5f;
    s[2] =  1.0f;
    s[3] = -1.0f;
    return OVE_OK;
}

static const struct ove_audio_node_ops f32_source_ops = {
    .configure = f32_source_configure,
    .process   = f32_source_process,
};

/* Sink that captures S16 data */
struct s16_sink_ctx {
    int16_t *captured;
    unsigned int count;
};

static int s16_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                              struct ove_audio_fmt *out)
{
    (void)ctx; (void)out;
    if (in->sample_fmt != OVE_AUDIO_FMT_S16)
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static int s16_sink_process(void *ctx, const struct ove_audio_buf *in,
                            struct ove_audio_buf *out)
{
    (void)out;
    struct s16_sink_ctx *ss = (struct s16_sink_ctx *)ctx;
    memcpy(ss->captured, in->data, in->frames * sizeof(int16_t));
    ss->count = in->frames;
    return OVE_OK;
}

static const struct ove_audio_node_ops s16_sink_ops = {
    .configure = s16_sink_configure,
    .process   = s16_sink_process,
};

static void test_converter_f32_to_s16(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &f32_source_ops, NULL, "f32-src",
                                       OVE_AUDIO_NODE_SOURCE);
    int conv = ove_audio_node_converter(&g, OVE_AUDIO_FMT_S16, "to-s16");

    int16_t cap[4] = {0};
    struct s16_sink_ctx ss = { .captured = cap };
    int sink = ove_audio_graph_add_node(&g, &s16_sink_ops, &ss, "s16-sink",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, conv);
    ove_audio_graph_connect(&g, conv, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* 0.5f * 32768 = 16384 */
    assert_true(abs(cap[0] - 16384) < 2);
    assert_true(abs(cap[1] - (-16384)) < 2);
    /* 1.0f should clamp to ~32767 */
    assert_true(cap[2] > 32000);
    assert_true(cap[3] < -32000);

    ove_audio_graph_deinit(&g);
}

/* ── Gain edge case: 0dB passthrough ────────────────────────────── */

static void test_gain_0db_passthrough(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4);

    int src = ove_audio_graph_add_node(&g, &counting_source_ops, NULL, "count",
                                       OVE_AUDIO_NODE_SOURCE);
    int gain = ove_audio_node_gain(&g, 0.0f, "unity"); /* 0 dB = 1.0x */

    int16_t captured[4] = {0};
    struct verify_sink_ctx vs = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &verify_sink_ops, &vs, "verify",
                                        OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, gain);
    ove_audio_graph_connect(&g, gain, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* 0dB gain: output should match source (0,1,2,3) exactly */
    for (int i = 0; i < 4; i++)
        assert_int_equal(captured[i], i);

    ove_audio_graph_deinit(&g);
}

/* ── Channel map: stereo→mono (left only) with 4 frames ─────────── */

/* Stereo source producing 4 frames: interleaved [L0,R0,L1,R1,L2,R2,L3,R3]
 * where sample[i] = i * 100 → L=0,R=100,L=200,R=300,L=400,R=500,L=600,R=700 */
static int stereo_source_4f_configure(void *ctx, const struct ove_audio_fmt *in,
                                      struct ove_audio_fmt *out)
{
    (void)ctx; (void)in;
    out->sample_rate = 48000;
    out->channels = 2;
    out->sample_fmt = OVE_AUDIO_FMT_S16;
    return OVE_OK;
}

static int stereo_source_4f_process(void *ctx, const struct ove_audio_buf *in,
                                    struct ove_audio_buf *out)
{
    (void)ctx; (void)in;
    int16_t *s = (int16_t *)out->data;
    /* Interleaved stereo: L0,R0,L1,R1,... */
    for (unsigned int i = 0; i < out->frames * 2; i++)
        s[i] = (int16_t)(i * 100); /* L=0,R=100,L=200,R=300,... */
    return OVE_OK;
}

static const struct ove_audio_node_ops stereo_source_4f_ops = {
    .configure = stereo_source_4f_configure,
    .process   = stereo_source_4f_process,
};

/* Mono capture sink: stores captured data and frame count */
struct mono_capture_ctx {
    int16_t *captured;
    unsigned int captured_frames;
};

static int mono_capture_process(void *ctx, const struct ove_audio_buf *in,
                                struct ove_audio_buf *out)
{
    (void)out;
    struct mono_capture_ctx *mc = (struct mono_capture_ctx *)ctx;
    memcpy(mc->captured, in->data, in->frames * sizeof(int16_t));
    mc->captured_frames = in->frames;
    return OVE_OK;
}

static int mono_capture_configure(void *ctx, const struct ove_audio_fmt *in,
                                  struct ove_audio_fmt *out)
{
    (void)ctx; (void)out;
    if (in->channels != 1)
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static const struct ove_audio_node_ops mono_capture_ops = {
    .configure = mono_capture_configure,
    .process   = mono_capture_process,
};

static void test_channel_map_stereo_to_mono_node(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4); /* 4 frames */

    int src = ove_audio_graph_add_node(&g, &stereo_source_4f_ops, NULL,
                                       "stereo-4f", OVE_AUDIO_NODE_SOURCE);

    /* Extract left channel (index 0) to mono */
    struct ove_audio_channel_map map = {
        .out_channels = 1,
        .map = { 0 }, /* out ch0 = in ch0 (left) */
    };
    int mapper = ove_audio_node_channel_map(&g, &map, "l-to-mono");

    int16_t captured[4] = {0};
    struct mono_capture_ctx mc = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &mono_capture_ops, &mc,
                                        "mono-cap", OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, mapper);
    ove_audio_graph_connect(&g, mapper, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* Source interleaved: [0,100,200,300,400,500,600,700]
     * Left channel (even indices): 0, 200, 400, 600 */
    assert_int_equal(mc.captured_frames, 4);
    assert_int_equal(captured[0], 0);
    assert_int_equal(captured[1], 200);
    assert_int_equal(captured[2], 400);
    assert_int_equal(captured[3], 600);

    ove_audio_graph_deinit(&g);
}

/* ── Channel map: silence channel ───────────────────────────────── */

static void test_channel_map_silence_channel(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    ove_audio_graph_init(&g, 4); /* 4 frames */

    int src = ove_audio_graph_add_node(&g, &stereo_source_4f_ops, NULL,
                                       "stereo-4f", OVE_AUDIO_NODE_SOURCE);

    /* Map output channel 0 to silence (-1) */
    struct ove_audio_channel_map map = {
        .out_channels = 1,
        .map = { -1 }, /* silence */
    };
    int mapper = ove_audio_node_channel_map(&g, &map, "silence");

    int16_t captured[4] = {99, 99, 99, 99};
    struct mono_capture_ctx mc = { .captured = captured };
    int sink = ove_audio_graph_add_node(&g, &mono_capture_ops, &mc,
                                        "mono-cap", OVE_AUDIO_NODE_SINK);

    ove_audio_graph_connect(&g, src, mapper);
    ove_audio_graph_connect(&g, mapper, sink);
    assert_int_equal(ove_audio_graph_build(&g), OVE_OK);
    assert_int_equal(ove_audio_graph_process(&g), OVE_OK);

    /* All samples should be zero (silence) */
    assert_int_equal(mc.captured_frames, 4);
    for (int i = 0; i < 4; i++)
        assert_int_equal(captured[i], 0);

    ove_audio_graph_deinit(&g);
}

/* ── Format equality tests ──────────────────────────────────────── */

static void test_fmt_equal_same(void **state)
{
    (void)state;
    struct ove_audio_fmt a = {
        .sample_rate = 48000,
        .channels = 2,
        .sample_fmt = OVE_AUDIO_FMT_S16,
    };
    struct ove_audio_fmt b = {
        .sample_rate = 48000,
        .channels = 2,
        .sample_fmt = OVE_AUDIO_FMT_S16,
    };
    assert_true(ove_audio_fmt_equal(&a, &b));
}

static void test_fmt_equal_different(void **state)
{
    (void)state;
    struct ove_audio_fmt base = {
        .sample_rate = 48000,
        .channels = 2,
        .sample_fmt = OVE_AUDIO_FMT_S16,
    };

    /* Different sample_rate */
    struct ove_audio_fmt diff_rate = base;
    diff_rate.sample_rate = 44100;
    assert_false(ove_audio_fmt_equal(&base, &diff_rate));

    /* Different channels */
    struct ove_audio_fmt diff_ch = base;
    diff_ch.channels = 1;
    assert_false(ove_audio_fmt_equal(&base, &diff_ch));

    /* Different sample_fmt */
    struct ove_audio_fmt diff_fmt = base;
    diff_fmt.sample_fmt = OVE_AUDIO_FMT_F32;
    assert_false(ove_audio_fmt_equal(&base, &diff_fmt));
}

/* ── setup/teardown ──────────────────────────────────────────────────── */

static int audio_setup(void **state)
{
    (void)state;
    memset(&g, 0, sizeof(g));
    start_call_count = 0;
    stop_call_count  = 0;
    fail_on_start_idx = -1;
    memset(start_call_order, 0, sizeof(start_call_order));
    memset(stop_call_order, 0, sizeof(stop_call_order));
    atomic_store(&tap_call_count, 0);
    return 0;
}

int test_audio_run(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_graph_init, audio_setup),
        cmocka_unit_test_setup(test_graph_init_null, audio_setup),
        cmocka_unit_test_setup(test_graph_init_zero_frames, audio_setup),
        cmocka_unit_test_setup(test_graph_add_node, audio_setup),
        cmocka_unit_test_setup(test_graph_connect, audio_setup),
        cmocka_unit_test_setup(test_graph_connect_sink_output_rejected, audio_setup),
        cmocka_unit_test_setup(test_graph_connect_source_input_rejected, audio_setup),
        cmocka_unit_test_setup(test_graph_build_simple, audio_setup),
        cmocka_unit_test_setup(test_graph_build_cycle_rejected, audio_setup),
        cmocka_unit_test_setup(test_graph_build_format_mismatch, audio_setup),
        cmocka_unit_test_setup(test_graph_data_flow, audio_setup),
        cmocka_unit_test_setup(test_graph_fan_out, audio_setup),
        cmocka_unit_test_setup(test_graph_node_error_silence, audio_setup),
        cmocka_unit_test_setup(test_converter_s16_to_f32, audio_setup),
        cmocka_unit_test_setup(test_channel_map_stereo_to_mono, audio_setup),
        cmocka_unit_test_setup(test_gain_node, audio_setup),
        cmocka_unit_test_setup(test_graph_gain_dataflow, audio_setup),
        cmocka_unit_test_setup(test_tap_node, audio_setup),
        /* State machine tests */
        cmocka_unit_test_setup(test_graph_start_stop, audio_setup),
        cmocka_unit_test_setup(test_graph_start_from_idle_fails, audio_setup),
        cmocka_unit_test_setup(test_graph_stop_from_idle_fails, audio_setup),
        cmocka_unit_test_setup(test_graph_start_failure_rollback, audio_setup),
        cmocka_unit_test_setup(test_graph_add_node_after_build_fails, audio_setup),
        cmocka_unit_test_setup(test_graph_connect_after_build_fails, audio_setup),
        /* Capacity tests */
        cmocka_unit_test_setup(test_graph_max_nodes, audio_setup),
        cmocka_unit_test_setup(test_graph_max_edges, audio_setup),
        /* Connect validation tests */
        cmocka_unit_test_setup(test_graph_connect_self_loop, audio_setup),
        cmocka_unit_test_setup(test_graph_connect_invalid_index, audio_setup),
        /* Stats tests */
        cmocka_unit_test_setup(test_graph_stats_cycle_count, audio_setup),
        cmocka_unit_test_setup(test_graph_stats_null_params, audio_setup),
        /* Multiple process cycles */
        cmocka_unit_test_setup(test_graph_process_multiple_cycles, audio_setup),
        /* Format conversion tests */
        cmocka_unit_test_setup(test_converter_f32_to_s16, audio_setup),
        /* Gain edge cases */
        cmocka_unit_test_setup(test_gain_0db_passthrough, audio_setup),
        /* Channel map tests */
        cmocka_unit_test_setup(test_channel_map_stereo_to_mono_node, audio_setup),
        cmocka_unit_test_setup(test_channel_map_silence_channel, audio_setup),
        /* Format equality tests */
        cmocka_unit_test_setup(test_fmt_equal_same, audio_setup),
        cmocka_unit_test_setup(test_fmt_equal_different, audio_setup),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}

#else /* !CONFIG_OVE_AUDIO */

int test_audio_run(void)
{
    return 0;
}

#endif /* CONFIG_OVE_AUDIO */
