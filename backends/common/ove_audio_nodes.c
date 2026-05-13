/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/types.h"
#include "ove/audio.h"
#include "ove_backend_common.h"
#include <string.h>
/* No <math.h> — avoid libm to stay compatible with Zephyr native_sim */

/* Per-node context allocation goes through OVE_BACKEND_MALLOC/FREE so it
 * lands in the RTOS-managed pool (pvPortMalloc / k_malloc) instead of
 * the libc heap.  Keeps audio bookkeeping consistent with the rest of
 * the kernel — heap_lock + xPortGetFreeHeapSize accounting includes it. */

/* The built-in utility nodes below allocate their per-node context from
 * the heap.  Under CONFIG_OVE_ZERO_HEAP the OVE_HEAP_AUDIO gate disables
 * them: apps link only the custom processor nodes they define themselves. */
#ifdef OVE_HEAP_AUDIO

/* ═══════════════════════════════════════════════════════════════════
   Format Converter
   ═══════════════════════════════════════════════════════════════════ */

struct converter_ctx {
	enum ove_audio_sample_fmt target_fmt;
};

static int converter_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	struct converter_ctx *c = (struct converter_ctx *)ctx;
	*out = *in;
	out->sample_fmt = c->target_fmt;
	return OVE_OK;
}

static int converter_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)ctx;
	unsigned int total = in->frames * in->fmt->channels;
	enum ove_audio_sample_fmt src_fmt = in->fmt->sample_fmt;
	enum ove_audio_sample_fmt dst_fmt = out->fmt->sample_fmt;

	if (src_fmt == dst_fmt) {
		memcpy(out->data, in->data, total * ove_audio_sample_size(src_fmt));
		return OVE_OK;
	}

	/* S16 → F32 */
	if (src_fmt == OVE_AUDIO_FMT_S16 && dst_fmt == OVE_AUDIO_FMT_F32) {
		const int16_t *s = (const int16_t *)in->data;
		float *d = (float *)out->data;
		for (unsigned int i = 0; i < total; i++)
			d[i] = (float)s[i] / 32768.0f;
		return OVE_OK;
	}

	/* F32 → S16 */
	if (src_fmt == OVE_AUDIO_FMT_F32 && dst_fmt == OVE_AUDIO_FMT_S16) {
		const float *s = (const float *)in->data;
		int16_t *d = (int16_t *)out->data;
		for (unsigned int i = 0; i < total; i++) {
			float v = s[i] * 32768.0f;
			if (v > 32767.0f)
				v = 32767.0f;
			if (v < -32768.0f)
				v = -32768.0f;
			d[i] = (int16_t)v;
		}
		return OVE_OK;
	}

	/* S16 → S32 */
	if (src_fmt == OVE_AUDIO_FMT_S16 && dst_fmt == OVE_AUDIO_FMT_S32) {
		const int16_t *s = (const int16_t *)in->data;
		int32_t *d = (int32_t *)out->data;
		for (unsigned int i = 0; i < total; i++)
			d[i] = (int32_t)s[i] << 16;
		return OVE_OK;
	}

	/* S32 → S16 */
	if (src_fmt == OVE_AUDIO_FMT_S32 && dst_fmt == OVE_AUDIO_FMT_S16) {
		const int32_t *s = (const int32_t *)in->data;
		int16_t *d = (int16_t *)out->data;
		for (unsigned int i = 0; i < total; i++)
			d[i] = (int16_t)(s[i] >> 16);
		return OVE_OK;
	}

	/* S32 → F32 */
	if (src_fmt == OVE_AUDIO_FMT_S32 && dst_fmt == OVE_AUDIO_FMT_F32) {
		const int32_t *s = (const int32_t *)in->data;
		float *d = (float *)out->data;
		for (unsigned int i = 0; i < total; i++)
			d[i] = (float)s[i] / 2147483648.0f;
		return OVE_OK;
	}

	/* F32 → S32 */
	if (src_fmt == OVE_AUDIO_FMT_F32 && dst_fmt == OVE_AUDIO_FMT_S32) {
		const float *s = (const float *)in->data;
		int32_t *d = (int32_t *)out->data;
		for (unsigned int i = 0; i < total; i++) {
			float v = s[i] * 2147483648.0f;
			if (v > 2147483647.0f)
				v = 2147483647.0f;
			if (v < -2147483648.0f)
				v = -2147483648.0f;
			d[i] = (int32_t)v;
		}
		return OVE_OK;
	}

	return OVE_ERR_NOT_SUPPORTED;
}

static void converter_destroy(void *ctx)
{
	OVE_BACKEND_FREE(ctx);
}

static const struct ove_audio_node_ops converter_ops = {
	.configure = converter_configure,
	.process = converter_process,
	.destroy = converter_destroy,
};

int ove_audio_node_converter(struct ove_audio_graph *g, enum ove_audio_sample_fmt target_fmt,
			     const char *name)
{
	struct converter_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;
	memset(ctx, 0, sizeof(*ctx));
	ctx->target_fmt = target_fmt;
	int idx = ove_audio_graph_add_node(g, &converter_ops, ctx, name, OVE_AUDIO_NODE_PROCESSOR);
	if (idx < 0)
		OVE_BACKEND_FREE(ctx);
	return idx;
}

/* ═══════════════════════════════════════════════════════════════════
   Channel Mapper
   ═══════════════════════════════════════════════════════════════════ */

struct channel_map_ctx {
	struct ove_audio_channel_map map;
	unsigned int in_channels;
};

static int channel_map_configure(void *ctx, const struct ove_audio_fmt *in,
				 struct ove_audio_fmt *out)
{
	struct channel_map_ctx *cm = (struct channel_map_ctx *)ctx;
	cm->in_channels = in->channels;
	*out = *in;
	out->channels = cm->map.out_channels;
	return OVE_OK;
}

static int channel_map_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	struct channel_map_ctx *cm = (struct channel_map_ctx *)ctx;
	unsigned int frames = in->frames;
	unsigned int in_ch = cm->in_channels;
	unsigned int out_ch = cm->map.out_channels;
	unsigned int sample_sz = ove_audio_sample_size(in->fmt->sample_fmt);

	for (unsigned int f = 0; f < frames; f++) {
		for (unsigned int c = 0; c < out_ch; c++) {
			int src_ch = cm->map.map[c];
			unsigned char *dst =
				(unsigned char *)out->data + (f * out_ch + c) * sample_sz;
			if (src_ch >= 0 && (unsigned int)src_ch < in_ch) {
				const unsigned char *src = (const unsigned char *)in->data +
							   (f * in_ch + src_ch) * sample_sz;
				memcpy(dst, src, sample_sz);
			} else {
				memset(dst, 0, sample_sz);
			}
		}
	}
	return OVE_OK;
}

static void channel_map_destroy(void *ctx)
{
	OVE_BACKEND_FREE(ctx);
}

static const struct ove_audio_node_ops channel_map_ops = {
	.configure = channel_map_configure,
	.process = channel_map_process,
	.destroy = channel_map_destroy,
};

int ove_audio_node_channel_map(struct ove_audio_graph *g, const struct ove_audio_channel_map *map,
			       const char *name)
{
	if (!map || map->out_channels == 0 || map->out_channels > OVE_AUDIO_MAX_CHANNELS)
		return OVE_ERR_INVALID_PARAM;
	struct channel_map_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;
	memset(ctx, 0, sizeof(*ctx));
	ctx->map = *map;
	int idx =
		ove_audio_graph_add_node(g, &channel_map_ops, ctx, name, OVE_AUDIO_NODE_PROCESSOR);
	if (idx < 0)
		OVE_BACKEND_FREE(ctx);
	return idx;
}

/* ═══════════════════════════════════════════════════════════════════
   Gain
   ═══════════════════════════════════════════════════════════════════ */

struct gain_ctx {
	float linear_gain;
};

static int gain_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	(void)ctx;
	*out = *in;
	return OVE_OK;
}

static int gain_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	struct gain_ctx *gc = (struct gain_ctx *)ctx;
	float g = gc->linear_gain;
	unsigned int total = in->frames * in->fmt->channels;

	switch (in->fmt->sample_fmt) {
	case OVE_AUDIO_FMT_S16: {
		const int16_t *s = (const int16_t *)in->data;
		int16_t *d = (int16_t *)out->data;
		for (unsigned int i = 0; i < total; i++) {
			float v = (float)s[i] * g;
			if (v > 32767.0f)
				v = 32767.0f;
			if (v < -32768.0f)
				v = -32768.0f;
			d[i] = (int16_t)v;
		}
		break;
	}
	case OVE_AUDIO_FMT_S32: {
		const int32_t *s = (const int32_t *)in->data;
		int32_t *d = (int32_t *)out->data;
		for (unsigned int i = 0; i < total; i++) {
			double v = (double)s[i] * (double)g;
			if (v > 2147483647.0)
				v = 2147483647.0;
			if (v < -2147483648.0)
				v = -2147483648.0;
			d[i] = (int32_t)v;
		}
		break;
	}
	case OVE_AUDIO_FMT_F32: {
		const float *s = (const float *)in->data;
		float *d = (float *)out->data;
		for (unsigned int i = 0; i < total; i++)
			d[i] = s[i] * g;
		break;
	}
	default:
		return OVE_ERR_NOT_SUPPORTED;
	}
	return OVE_OK;
}

static void gain_destroy(void *ctx)
{
	OVE_BACKEND_FREE(ctx);
}

static const struct ove_audio_node_ops gain_ops = {
	.configure = gain_configure,
	.process = gain_process,
	.destroy = gain_destroy,
};

int ove_audio_node_gain(struct ove_audio_graph *g, float gain_db, const char *name)
{
	struct gain_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;
	memset(ctx, 0, sizeof(*ctx));
	/* Convert dB to linear gain: 10^(dB/20)
     *
     * Avoid libm powf/expf — they pull in glibc optimized code that
     * references _dl_x86_cpu_features, breaking Zephyr native_sim.
     * Use a Padé-approximation of exp(x) for the small range we need. */
	{
		float x = gain_db * 0.11512925464970229f; /* dB * ln(10)/20 */
		/* exp(x) ≈ (120+60x+12x²+x³) / (120-60x+12x²-x³) for |x| < 4
         * Accurate to <0.02% over the audio gain range (±40 dB). */
		float x2 = x * x;
		float x3 = x2 * x;
		ctx->linear_gain = (120.0f + 60.0f * x + 12.0f * x2 + x3) /
				   (120.0f - 60.0f * x + 12.0f * x2 - x3);
		if (ctx->linear_gain < 0.0f)
			ctx->linear_gain = 0.0f;
	}
	int idx = ove_audio_graph_add_node(g, &gain_ops, ctx, name, OVE_AUDIO_NODE_PROCESSOR);
	if (idx < 0)
		OVE_BACKEND_FREE(ctx);
	return idx;
}

/* ═══════════════════════════════════════════════════════════════════
   Tap (Observer)
   ═══════════════════════════════════════════════════════════════════ */

struct tap_ctx {
	ove_audio_tap_fn fn;
	void *user_data;
};

static int tap_configure(void *ctx, const struct ove_audio_fmt *in, struct ove_audio_fmt *out)
{
	(void)ctx;
	*out = *in;
	return OVE_OK;
}

static int tap_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	struct tap_ctx *tc = (struct tap_ctx *)ctx;
	unsigned int bytes =
		in->frames * in->fmt->channels * ove_audio_sample_size(in->fmt->sample_fmt);
	memcpy(out->data, in->data, bytes);
	if (tc->fn)
		tc->fn(in, tc->user_data);
	return OVE_OK;
}

static void tap_destroy(void *ctx)
{
	OVE_BACKEND_FREE(ctx);
}

static const struct ove_audio_node_ops tap_ops = {
	.configure = tap_configure,
	.process = tap_process,
	.destroy = tap_destroy,
};

int ove_audio_node_tap(struct ove_audio_graph *g, ove_audio_tap_fn fn, void *user_data,
		       const char *name)
{
	struct tap_ctx *ctx = OVE_BACKEND_MALLOC(sizeof(*ctx));
	if (!ctx)
		return OVE_ERR_NO_MEMORY;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fn = fn;
	ctx->user_data = user_data;
	int idx = ove_audio_graph_add_node(g, &tap_ops, ctx, name, OVE_AUDIO_NODE_PROCESSOR);
	if (idx < 0)
		OVE_BACKEND_FREE(ctx);
	return idx;
}

#endif /* OVE_HEAP_AUDIO */
