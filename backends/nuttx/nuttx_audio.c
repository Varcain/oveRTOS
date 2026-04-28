/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/types.h"
#include "ove/audio_device.h"
#include "ove_backend_common.h"

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <errno.h>
#include <mqueue.h>
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>
#include <nuttx/queue.h>

#ifdef CONFIG_OVE_AUDIO

#define AUDIO_APB_RECORD (1 << 4)
#define AUDIO_APB_PLAY (1 << 5)

#define AUDIO_PLAY_PATH "/dev/audio/pcm0"
#define AUDIO_REC_PATH "/dev/audio/pcm1"
#define DEFAULT_NUM_BUFFERS 3
#define DEFAULT_AUDIO_PRIORITY 200
#define DEFAULT_AUDIO_STACK 4096
#define BYTES_PER_SAMPLE sizeof(int16_t)
/* Upper bound on buffer-pointer arrays so they can live on the audio
 * pthread's stack rather than the heap.  Matches configs seen in practice
 * and is enforced at audio graph setup. */
#define MAX_NUM_BUFFERS 16

/* ========================================================================= */
/* SOURCE CONTEXT                                                            */
/* ========================================================================= */

struct nuttx_source_ctx {
	struct ove_audio_fmt fmt;
	/* Stashed RX buffer pointer — set by the audio pthread before calling
     * ove_audio_graph_process(), consumed by nuttx_source_process(). */
	const int16_t *rx_ptr;
	unsigned int rx_frames;
};

/* Single static source context — only one NuttX audio graph at a time */
static struct nuttx_source_ctx g_source_ctx;

/* ========================================================================= */
/* SINK CONTEXT                                                              */
/* ========================================================================= */

struct nuttx_sink_ctx {
	struct ove_audio_fmt fmt;
	struct ove_audio_graph *graph;
	unsigned int frames_per_period;
	unsigned int num_buffers;
	unsigned int thread_priority;
	unsigned int thread_stack_size;

	/* NuttX audio resources — owned by the sink */
	int record_fd;
	int play_fd;
	mqd_t audio_mq;
	pthread_t audio_tid;

	/* Stashed TX buffer pointer — set by the audio pthread before calling
     * ove_audio_graph_process(), consumed by nuttx_sink_process(). */
	int16_t *tx_ptr;
	unsigned int tx_frames;
};

/* Single static sink context — only one NuttX audio graph at a time */
static struct nuttx_sink_ctx g_sink_ctx;

/* ========================================================================= */
/* AUDIO PTHREAD                                                             */
/* ========================================================================= */

static void *audio_thread_fn(void *arg)
{
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)arg;
	struct audio_buf_desc_s buf_desc;
	struct ap_buffer_info_s rec_buf_info;
	struct ap_buffer_info_s play_buf_info;
	/* Per-buffer pointer arrays live on the audio-thread stack (not the
     * heap), so the backend works unchanged under CONFIG_OVE_ZERO_HEAP.
     * The MAX_NUM_BUFFERS cap is enforced when the sink is created. */
	struct ap_buffer_s *rec_bufs[MAX_NUM_BUFFERS] = {0};
	struct ap_buffer_s *play_bufs[MAX_NUM_BUFFERS] = {0};
	struct audio_msg_s msg;
	unsigned int prio;
	int rec_nbuffers;
	int play_nbuffers;
	int rec_buf_size;
	int play_buf_size;
	int block_size;
	int ret;
	int i;

	block_size = (int)(sc->frames_per_period * BYTES_PER_SAMPLE);

	/* Query record buffer info */

	ret = ioctl(sc->record_fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&rec_buf_info);
	if (ret != OK) {
		rec_buf_info.buffer_size = (uint32_t)block_size;
		rec_buf_info.nbuffers = sc->num_buffers;
	}

	rec_nbuffers = (int)rec_buf_info.nbuffers;
	rec_buf_size = (int)rec_buf_info.buffer_size;
	if (rec_nbuffers > (int)sc->num_buffers) {
		rec_nbuffers = (int)sc->num_buffers;
	}
	if (rec_nbuffers > MAX_NUM_BUFFERS) {
		rec_nbuffers = MAX_NUM_BUFFERS;
	}

	/* Query playback buffer info */

	ret = ioctl(sc->play_fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&play_buf_info);
	if (ret != OK) {
		play_buf_info.buffer_size = (uint32_t)block_size;
		play_buf_info.nbuffers = sc->num_buffers;
	}

	play_nbuffers = (int)play_buf_info.nbuffers;
	play_buf_size = (int)play_buf_info.buffer_size;
	if (play_nbuffers > (int)sc->num_buffers) {
		play_nbuffers = (int)sc->num_buffers;
	}
	if (play_nbuffers > MAX_NUM_BUFFERS) {
		play_nbuffers = MAX_NUM_BUFFERS;
	}

	/* rec_bufs / play_bufs are stack arrays sized to MAX_NUM_BUFFERS; no
     * heap allocation needed.  The num-buffers cap was enforced at sink
     * creation, so the counts here always fit. */

	/* Allocate NuttX audio buffers */

	for (i = 0; i < rec_nbuffers; i++) {
		buf_desc.numbytes = rec_buf_size;
		buf_desc.u.pbuffer = &rec_bufs[i];
		ret = ioctl(sc->record_fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc);
		if (ret != sizeof(buf_desc)) {
			goto err_out;
		}
	}

	for (i = 0; i < play_nbuffers; i++) {
		buf_desc.numbytes = play_buf_size;
		buf_desc.u.pbuffer = &play_bufs[i];
		ret = ioctl(sc->play_fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc);
		if (ret != sizeof(buf_desc)) {
			goto err_out;
		}
	}

	/* Initialise dequeues */

	struct dq_queue_s play_avail;
	struct dq_queue_s rec_done;
	dq_init(&play_avail);
	dq_init(&rec_done);

	/* Pre-fill playback with silence and start */

	for (i = 0; i < play_nbuffers; i++) {
		memset(play_bufs[i]->samp, 0, (size_t)play_buf_size);
		play_bufs[i]->nbytes = (uint32_t)play_buf_size;
		play_bufs[i]->curbyte = 0;
		play_bufs[i]->flags = AUDIO_APB_PLAY;
		buf_desc.numbytes = play_buf_size;
		buf_desc.u.buffer = play_bufs[i];
		ioctl(sc->play_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
	}

	ioctl(sc->play_fd, AUDIOIOC_START, 0);

	/* Enqueue record buffers and start recording */

	for (i = 0; i < rec_nbuffers; i++) {
		rec_bufs[i]->nbytes = (uint32_t)rec_buf_size;
		rec_bufs[i]->curbyte = 0;
		rec_bufs[i]->flags = AUDIO_APB_RECORD;
		buf_desc.numbytes = rec_buf_size;
		buf_desc.u.buffer = rec_bufs[i];
		ioctl(sc->record_fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
	}

	ioctl(sc->record_fd, AUDIOIOC_START, 0);

	printf("NuttX audio thread running\n");

	/* Main audio processing loop */

	for (;;) {
		ssize_t size;

		size = mq_receive(sc->audio_mq, (char *)&msg, sizeof(msg), &prio);
		if (size != sizeof(msg)) {
			continue;
		}

		if (msg.msg_id == AUDIO_MSG_DEQUEUE) {
			struct ap_buffer_s *apb = (struct ap_buffer_s *)msg.u.ptr;
			bool is_play = false;
			bool is_rec = false;
			int j;

			for (j = 0; j < play_nbuffers; j++) {
				if (apb == play_bufs[j]) {
					is_play = true;
					break;
				}
			}

			if (!is_play) {
				for (j = 0; j < rec_nbuffers; j++) {
					if (apb == rec_bufs[j]) {
						is_rec = true;
						break;
					}
				}
			}

			if (is_play) {
				apb->curbyte = 0;
				dq_addlast(&apb->dq_entry, &play_avail);
			} else if (is_rec) {
				apb->curbyte = 0;
				dq_addlast(&apb->dq_entry, &rec_done);
			}

			/* Process all paired RX/TX buffers */

			while (dq_count(&rec_done) > 0 && dq_count(&play_avail) > 0) {
				struct ap_buffer_s *rec_apb =
					(struct ap_buffer_s *)dq_remfirst(&rec_done);
				struct ap_buffer_s *play_apb =
					(struct ap_buffer_s *)dq_remfirst(&play_avail);

				unsigned int nsamples = rec_apb->nbytes / BYTES_PER_SAMPLE;
				if (nsamples > sc->frames_per_period) {
					nsamples = sc->frames_per_period;
				}

				/* Stash buffer pointers for source/sink process() calls */

				g_source_ctx.rx_ptr = (const int16_t *)rec_apb->samp;
				g_source_ctx.rx_frames = nsamples;
				sc->tx_ptr = (int16_t *)play_apb->samp;
				sc->tx_frames = nsamples;

				/* Drive the entire graph: source copies RX, processors
                 * transform, sink writes TX */

				ove_audio_graph_process(sc->graph);

				/* Clear stashed pointers */

				g_source_ctx.rx_ptr = NULL;
				sc->tx_ptr = NULL;

				/* Re-enqueue record buffer */

				rec_apb->nbytes = (uint32_t)rec_buf_size;
				rec_apb->curbyte = 0;
				rec_apb->flags = AUDIO_APB_RECORD;
				buf_desc.numbytes = rec_buf_size;
				buf_desc.u.buffer = rec_apb;
				ioctl(sc->record_fd, AUDIOIOC_ENQUEUEBUFFER,
				      (unsigned long)&buf_desc);

				/* Enqueue playback buffer with processed audio */

				play_apb->nbytes = (uint32_t)(nsamples * BYTES_PER_SAMPLE);
				play_apb->curbyte = 0;
				play_apb->flags = AUDIO_APB_PLAY;
				buf_desc.numbytes = (int)play_apb->nbytes;
				buf_desc.u.buffer = play_apb;
				ioctl(sc->play_fd, AUDIOIOC_ENQUEUEBUFFER,
				      (unsigned long)&buf_desc);
			}
		}
	}

err_out:
	for (i = 0; i < rec_nbuffers; i++) {
		if (rec_bufs[i] != NULL) {
			buf_desc.u.buffer = rec_bufs[i];
			ioctl(sc->record_fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
		}
	}

	for (i = 0; i < play_nbuffers; i++) {
		if (play_bufs[i] != NULL) {
			buf_desc.u.buffer = play_bufs[i];
			ioctl(sc->play_fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
		}
	}

	return NULL;
}

/* ========================================================================= */
/* SOURCE NODE OPS                                                           */
/* ========================================================================= */

static int nuttx_source_configure(void *ctx, const struct ove_audio_fmt *in,
				  struct ove_audio_fmt *out)
{
	(void)in;
	struct nuttx_source_ctx *sc = (struct nuttx_source_ctx *)ctx;
	*out = sc->fmt;
	return OVE_OK;
}

static int nuttx_source_process(void *ctx, const struct ove_audio_buf *in,
				struct ove_audio_buf *out)
{
	(void)in;
	struct nuttx_source_ctx *sc = (struct nuttx_source_ctx *)ctx;

	if (sc->rx_ptr == NULL || out == NULL || out->data == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	unsigned int frames = out->frames;
	if (frames > sc->rx_frames) {
		frames = sc->rx_frames;
	}

	unsigned int bytes =
		frames * out->fmt->channels * ove_audio_sample_size(out->fmt->sample_fmt);
	memcpy(out->data, sc->rx_ptr, bytes);
	return OVE_OK;
}

static const struct ove_audio_node_ops nuttx_source_ops = {
	.configure = nuttx_source_configure,
	.process = nuttx_source_process,
};

/* ========================================================================= */
/* SINK NODE OPS                                                             */
/* ========================================================================= */

static int nuttx_sink_configure(void *ctx, const struct ove_audio_fmt *in,
				struct ove_audio_fmt *out)
{
	(void)out;
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)ctx;
	if (!ove_audio_fmt_equal(in, &sc->fmt)) {
		return OVE_ERR_INVALID_PARAM;
	}
	return OVE_OK;
}

static int nuttx_sink_start(void *ctx)
{
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)ctx;
	struct audio_caps_desc_s cap_desc;
	struct mq_attr mq_attr_s;
	pthread_attr_t attr;
	struct sched_param param;
	int ret;

	/* Open record device */

	sc->record_fd = open(AUDIO_REC_PATH, O_RDONLY);
	if (sc->record_fd < 0) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	/* Open playback device */

	sc->play_fd = open(AUDIO_PLAY_PATH, O_WRONLY);
	if (sc->play_fd < 0) {
		close(sc->record_fd);
		sc->record_fd = -1;
		return OVE_ERR_NOT_SUPPORTED;
	}

	ioctl(sc->record_fd, AUDIOIOC_RESERVE, 0);
	ioctl(sc->play_fd, AUDIOIOC_RESERVE, 0);

	/* Configure recording */

	memset(&cap_desc, 0, sizeof(cap_desc));
	cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
	cap_desc.caps.ac_type = AUDIO_TYPE_INPUT;
	cap_desc.caps.ac_channels = (uint8_t)sc->fmt.channels;
	cap_desc.caps.ac_controls.hw[0] = (uint16_t)sc->fmt.sample_rate;
	cap_desc.caps.ac_controls.b[2] = (uint8_t)(BYTES_PER_SAMPLE * 8);

	ret = ioctl(sc->record_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
	if (ret < 0) {
		printf("NuttX: failed to configure audio input: %d\n", errno);
	}

	/* Configure playback */

	memset(&cap_desc, 0, sizeof(cap_desc));
	cap_desc.caps.ac_len = sizeof(struct audio_caps_s);
	cap_desc.caps.ac_type = AUDIO_TYPE_OUTPUT;
	cap_desc.caps.ac_channels = (uint8_t)sc->fmt.channels;
	cap_desc.caps.ac_controls.hw[0] = (uint16_t)sc->fmt.sample_rate;
	cap_desc.caps.ac_controls.b[2] = (uint8_t)(BYTES_PER_SAMPLE * 8);

	ret = ioctl(sc->play_fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc);
	if (ret < 0) {
		printf("NuttX: failed to configure audio output: %d\n", errno);
	}

	/* Create message queue */

	mq_attr_s.mq_maxmsg = sc->num_buffers * 4;
	mq_attr_s.mq_msgsize = sizeof(struct audio_msg_s);
	mq_attr_s.mq_curmsgs = 0;
	mq_attr_s.mq_flags = 0;

	sc->audio_mq =
		mq_open("/" CONFIG_OVE_APP_NAME "_audio", O_RDWR | O_CREAT, 0644, &mq_attr_s);
	if (sc->audio_mq == (mqd_t)-1) {
		close(sc->record_fd);
		close(sc->play_fd);
		sc->record_fd = -1;
		sc->play_fd = -1;
		return OVE_ERR_NOT_SUPPORTED;
	}

	ioctl(sc->record_fd, AUDIOIOC_REGISTERMQ, (unsigned long)sc->audio_mq);
	ioctl(sc->play_fd, AUDIOIOC_REGISTERMQ, (unsigned long)sc->audio_mq);

	/* Spawn audio pthread */

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, sc->thread_stack_size);
	param.sched_priority = (int)sc->thread_priority;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

	ret = pthread_create(&sc->audio_tid, &attr, audio_thread_fn, sc);
	pthread_attr_destroy(&attr);

	if (ret != 0) {
		mq_close(sc->audio_mq);
		mq_unlink("/" CONFIG_OVE_APP_NAME "_audio");
		close(sc->record_fd);
		close(sc->play_fd);
		sc->audio_mq = (mqd_t)-1;
		sc->record_fd = -1;
		sc->play_fd = -1;
		return OVE_ERR_NO_MEMORY;
	}

	pthread_setname_np(sc->audio_tid, CONFIG_OVE_APP_NAME "_audio");
	return OVE_OK;
}

static int nuttx_sink_stop(void *ctx)
{
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)ctx;

	if (sc->record_fd >= 0) {
		ioctl(sc->record_fd, AUDIOIOC_STOP, 0);
	}

	if (sc->play_fd >= 0) {
		ioctl(sc->play_fd, AUDIOIOC_STOP, 0);
	}

	return OVE_OK;
}

static int nuttx_sink_process(void *ctx, const struct ove_audio_buf *in, struct ove_audio_buf *out)
{
	(void)out;
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)ctx;

	if (sc->tx_ptr == NULL || in == NULL || in->data == NULL) {
		return OVE_ERR_INVALID_PARAM;
	}

	unsigned int frames = in->frames;
	if (frames > sc->tx_frames) {
		frames = sc->tx_frames;
	}

	unsigned int bytes =
		frames * in->fmt->channels * ove_audio_sample_size(in->fmt->sample_fmt);
	memcpy(sc->tx_ptr, in->data, bytes);
	return OVE_OK;
}

static void nuttx_sink_destroy(void *ctx)
{
	struct nuttx_sink_ctx *sc = (struct nuttx_sink_ctx *)ctx;

	if (sc->audio_mq != (mqd_t)-1) {
		mq_close(sc->audio_mq);
		mq_unlink("/" CONFIG_OVE_APP_NAME "_audio");
		sc->audio_mq = (mqd_t)-1;
	}

	if (sc->record_fd >= 0) {
		close(sc->record_fd);
		sc->record_fd = -1;
	}

	if (sc->play_fd >= 0) {
		close(sc->play_fd);
		sc->play_fd = -1;
	}
}

static const struct ove_audio_node_ops nuttx_sink_ops = {
	.configure = nuttx_sink_configure,
	.start = nuttx_sink_start,
	.stop = nuttx_sink_stop,
	.process = nuttx_sink_process,
	.destroy = nuttx_sink_destroy,
};

/* ========================================================================= */
/* DEVICE NODE FACTORIES                                                     */
/* ========================================================================= */

int ove_audio_device_source(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			    const char *name)
{
	if (!g || !cfg || !name) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	struct nuttx_source_ctx *ctx = &g_source_ctx;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;

	int idx = ove_audio_graph_add_node(g, &nuttx_source_ops, ctx, name, OVE_AUDIO_NODE_SOURCE);
	return idx;
}

int ove_audio_device_sink(struct ove_audio_graph *g, const struct ove_audio_device_cfg *cfg,
			  const char *name)
{
	if (!g || !cfg || !name) {
		return OVE_ERR_INVALID_PARAM;
	}

	if (cfg->transport != OVE_AUDIO_TRANSPORT_I2S) {
		return OVE_ERR_NOT_SUPPORTED;
	}

	struct nuttx_sink_ctx *ctx = &g_sink_ctx;
	memset(ctx, 0, sizeof(*ctx));
	ctx->fmt = cfg->fmt;
	ctx->graph = g;
	ctx->frames_per_period = g->frames_per_period;
	ctx->num_buffers = cfg->num_buffers ? cfg->num_buffers : DEFAULT_NUM_BUFFERS;
	if (ctx->num_buffers > MAX_NUM_BUFFERS) {
		ctx->num_buffers = MAX_NUM_BUFFERS;
	}
	ctx->thread_priority = cfg->thread_priority ? cfg->thread_priority : DEFAULT_AUDIO_PRIORITY;
	ctx->thread_stack_size = cfg->thread_stack_size ? cfg->thread_stack_size
							: DEFAULT_AUDIO_STACK;
	ctx->record_fd = -1;
	ctx->play_fd = -1;
	ctx->audio_mq = (mqd_t)-1;

	int idx = ove_audio_graph_add_node(g, &nuttx_sink_ops, ctx, name, OVE_AUDIO_NODE_SINK);
	return idx;
}

#endif /* CONFIG_OVE_AUDIO */
