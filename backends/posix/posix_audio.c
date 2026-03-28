/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/types.h"
#include "ove/audio_device.h"

#ifdef CONFIG_OVE_AUDIO

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════
   SDL2 Source Node
   ═══════════════════════════════════════════════════════════════════ */

struct sdl_source_ctx {
    struct ove_audio_fmt    fmt;
    SDL_AudioDeviceID       dev;
    const char             *device_name;
    struct ove_audio_graph *graph;  /* for stats.overruns */
};

static int sdl_source_configure(void *ctx, const struct ove_audio_fmt *in,
                                struct ove_audio_fmt *out)
{
    (void)in;
    struct sdl_source_ctx *sc = (struct sdl_source_ctx *)ctx;
    *out = sc->fmt;
    return OVE_OK;
}

static SDL_AudioFormat sdl_fmt_from_ove(enum ove_audio_sample_fmt fmt)
{
    switch (fmt) {
    case OVE_AUDIO_FMT_S16: return AUDIO_S16SYS;
    case OVE_AUDIO_FMT_S32: return AUDIO_S32SYS;
    case OVE_AUDIO_FMT_F32: return AUDIO_F32SYS;
    default:                 return AUDIO_S16SYS;
    }
}

static int sdl_source_start(void *ctx)
{
    struct sdl_source_ctx *sc = (struct sdl_source_ctx *)ctx;

    SDL_AudioSpec want = {0};
    want.freq     = (int)sc->fmt.sample_rate;
    want.format   = sdl_fmt_from_ove(sc->fmt.sample_fmt);
    want.channels = (Uint8)sc->fmt.channels;
    want.samples  = 1024;

    sc->dev = SDL_OpenAudioDevice(sc->device_name, 1 /* capture */,
                                  &want, NULL, 0);
    if (sc->dev == 0)
        return OVE_ERR_NOT_SUPPORTED;

    SDL_PauseAudioDevice(sc->dev, 0);
    return OVE_OK;
}

static int sdl_source_stop(void *ctx)
{
    struct sdl_source_ctx *sc = (struct sdl_source_ctx *)ctx;
    if (sc->dev) {
        SDL_PauseAudioDevice(sc->dev, 1);
        SDL_CloseAudioDevice(sc->dev);
        sc->dev = 0;
    }
    return OVE_OK;
}

static int sdl_source_process(void *ctx, const struct ove_audio_buf *in,
                              struct ove_audio_buf *out)
{
    (void)in;
    struct sdl_source_ctx *sc = (struct sdl_source_ctx *)ctx;
    unsigned int bytes = out->frames * out->fmt->channels *
                         ove_audio_sample_size(out->fmt->sample_fmt);

    if (sc->dev) {
        Uint32 avail = SDL_DequeueAudio(sc->dev, out->data, bytes);
        if (avail < bytes) {
            memset((char *)out->data + avail, 0, bytes - avail);
            if (sc->graph)
                sc->graph->stats.overruns++;
        }
    } else {
        memset(out->data, 0, bytes);
    }

    return OVE_OK;
}

static void sdl_source_destroy(void *ctx)
{
    struct sdl_source_ctx *sc = (struct sdl_source_ctx *)ctx;
    if (sc->dev)
        SDL_CloseAudioDevice(sc->dev);
    free(sc);
}

static const struct ove_audio_node_ops sdl_source_ops = {
    .configure = sdl_source_configure,
    .start     = sdl_source_start,
    .stop      = sdl_source_stop,
    .process   = sdl_source_process,
    .destroy   = sdl_source_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
   SDL2 Sink Node
   ═══════════════════════════════════════════════════════════════════ */

struct sdl_sink_ctx {
    struct ove_audio_fmt     fmt;
    SDL_AudioDeviceID        dev;
    const char              *device_name;
    struct ove_audio_graph  *graph;
    unsigned int             frames_per_period;
    /* Set by SDL callback before graph_process(), read by process() */
    Uint8                   *sdl_stream;
    unsigned int             sdl_stream_len;
};

static int sdl_sink_configure(void *ctx, const struct ove_audio_fmt *in,
                              struct ove_audio_fmt *out)
{
    (void)out;
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)ctx;
    if (!ove_audio_fmt_equal(in, &sc->fmt))
        return OVE_ERR_INVALID_PARAM;
    return OVE_OK;
}

static void sdl_sink_callback(void *userdata, Uint8 *stream, int len)
{
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)userdata;
    memset(stream, 0, (size_t)len); /* default silence */
    sc->sdl_stream = stream;
    sc->sdl_stream_len = (unsigned int)len;
    ove_audio_graph_process(sc->graph);
    sc->sdl_stream = NULL;
}

static int sdl_sink_start(void *ctx)
{
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)ctx;

    SDL_AudioSpec want = {0};
    want.freq     = (int)sc->fmt.sample_rate;
    want.format   = sdl_fmt_from_ove(sc->fmt.sample_fmt);
    want.channels = (Uint8)sc->fmt.channels;
    want.samples  = (Uint16)sc->frames_per_period;
    want.callback = sdl_sink_callback;
    want.userdata = sc;

    sc->dev = SDL_OpenAudioDevice(sc->device_name, 0 /* playback */,
                                  &want, NULL, 0);
    if (sc->dev == 0)
        return OVE_ERR_NOT_SUPPORTED;

    SDL_PauseAudioDevice(sc->dev, 0);
    return OVE_OK;
}

static int sdl_sink_stop(void *ctx)
{
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)ctx;
    if (sc->dev) {
        SDL_PauseAudioDevice(sc->dev, 1);
        SDL_CloseAudioDevice(sc->dev);
        sc->dev = 0;
    }
    return OVE_OK;
}

static int sdl_sink_process(void *ctx, const struct ove_audio_buf *in,
                            struct ove_audio_buf *out)
{
    (void)out;
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)ctx;
    if (sc->sdl_stream && in && in->data) {
        unsigned int bytes = in->frames * in->fmt->channels *
                             ove_audio_sample_size(in->fmt->sample_fmt);
        if (bytes > sc->sdl_stream_len)
            bytes = sc->sdl_stream_len;
        memcpy(sc->sdl_stream, in->data, bytes);
    }
    return OVE_OK;
}

static void sdl_sink_destroy(void *ctx)
{
    struct sdl_sink_ctx *sc = (struct sdl_sink_ctx *)ctx;
    if (sc->dev)
        SDL_CloseAudioDevice(sc->dev);
    free(sc);
}

static const struct ove_audio_node_ops sdl_sink_ops = {
    .configure = sdl_sink_configure,
    .start     = sdl_sink_start,
    .stop      = sdl_sink_stop,
    .process   = sdl_sink_process,
    .destroy   = sdl_sink_destroy,
};

/* ═══════════════════════════════════════════════════════════════════
   Device Node Factories
   ═══════════════════════════════════════════════════════════════════ */

static int sdl_init_once(void)
{
    static int initialized = 0;
    if (!initialized) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
            return OVE_ERR_NOT_SUPPORTED;
        initialized = 1;
    }
    return OVE_OK;
}

int ove_audio_device_source(struct ove_audio_graph *g,
                            const struct ove_audio_device_cfg *cfg,
                            const char *name)
{
    if (!g || !cfg || !name)
        return OVE_ERR_INVALID_PARAM;

    if (cfg->transport != OVE_AUDIO_TRANSPORT_SDL2)
        return OVE_ERR_NOT_SUPPORTED;

    int ret = sdl_init_once();
    if (ret != OVE_OK)
        return ret;

    struct sdl_source_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return OVE_ERR_NO_MEMORY;

    ctx->fmt = cfg->fmt;
    ctx->device_name = cfg->sdl2.device_name;
    ctx->graph = g;

    int idx = ove_audio_graph_add_node(g, &sdl_source_ops, ctx, name,
                                       OVE_AUDIO_NODE_SOURCE);
    if (idx < 0)
        free(ctx);
    return idx;
}

int ove_audio_device_sink(struct ove_audio_graph *g,
                          const struct ove_audio_device_cfg *cfg,
                          const char *name)
{
    if (!g || !cfg || !name)
        return OVE_ERR_INVALID_PARAM;

    if (cfg->transport != OVE_AUDIO_TRANSPORT_SDL2)
        return OVE_ERR_NOT_SUPPORTED;

    int ret = sdl_init_once();
    if (ret != OVE_OK)
        return ret;

    struct sdl_sink_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return OVE_ERR_NO_MEMORY;

    ctx->fmt = cfg->fmt;
    ctx->device_name = cfg->sdl2.device_name;
    ctx->graph = g;
    ctx->frames_per_period = g->frames_per_period;

    int idx = ove_audio_graph_add_node(g, &sdl_sink_ops, ctx, name,
                                       OVE_AUDIO_NODE_SINK);
    if (idx < 0)
        free(ctx);
    return idx;
}

#endif /* CONFIG_OVE_AUDIO */
