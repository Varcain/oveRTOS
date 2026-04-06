/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * In-process direct transport for POSIX simulation mode.
 *
 * Uses a pair of SPSC ring buffers (events: firmware -> WS server,
 * commands: WS server -> firmware) protected by a pthread mutex
 * and condition variable for wakeup.
 */

#include "ove/sim/ove_sim_transport.h"
#include "ove/types.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Ring buffer ───────────────────────────────────────────────────── */

#define RING_SIZE (1u << 16) /* 64 KB */

struct ring {
	uint8_t        buf[RING_SIZE];
	uint32_t       write_pos;
	uint32_t       read_pos;
	pthread_mutex_t lock;
	pthread_cond_t  cond;
};

static void ring_init(struct ring *r)
{
	memset(r->buf, 0, RING_SIZE);
	r->write_pos = 0;
	r->read_pos = 0;
	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->cond, NULL);
}

static void ring_destroy(struct ring *r)
{
	pthread_cond_destroy(&r->cond);
	pthread_mutex_destroy(&r->lock);
}

static uint32_t ring_available(const struct ring *r)
{
	return r->write_pos - r->read_pos;
}

static uint32_t ring_free(const struct ring *r)
{
	return RING_SIZE - ring_available(r);
}

/**
 * Write a length-prefixed message into the ring.
 * Format: [uint16_t len][data[len]]
 */
static int ring_write(struct ring *r, const void *data, uint16_t len)
{
	uint32_t total = (uint32_t)sizeof(uint16_t) + len;

	pthread_mutex_lock(&r->lock);

	if (ring_free(r) < total) {
		pthread_mutex_unlock(&r->lock);
		return OVE_ERR_QUEUE_FULL;
	}

	/* Write length prefix. */
	uint32_t pos = r->write_pos & (RING_SIZE - 1);
	r->buf[pos] = (uint8_t)(len & 0xFF);
	pos = (pos + 1) & (RING_SIZE - 1);
	r->buf[pos] = (uint8_t)((len >> 8) & 0xFF);
	pos = (pos + 1) & (RING_SIZE - 1);

	/* Write payload. */
	const uint8_t *src = (const uint8_t *)data;
	for (uint16_t i = 0; i < len; i++) {
		r->buf[pos] = src[i];
		pos = (pos + 1) & (RING_SIZE - 1);
	}

	r->write_pos += total;

	pthread_cond_signal(&r->cond);
	pthread_mutex_unlock(&r->lock);
	return OVE_OK;
}

/**
 * Read a length-prefixed message from the ring.
 * Returns 0 on success, OVE_ERR_TIMEOUT if no message within timeout.
 */
static int ring_read(struct ring *r, void *buf, size_t buf_size,
		     uint16_t *out_len, uint32_t timeout_ms)
{
	pthread_mutex_lock(&r->lock);

	/* Wait for data. */
	while (ring_available(r) < sizeof(uint16_t)) {
		if (timeout_ms == 0) {
			pthread_mutex_unlock(&r->lock);
			return OVE_ERR_TIMEOUT;
		}

		if (timeout_ms == UINT32_MAX) {
			pthread_cond_wait(&r->cond, &r->lock);
		} else {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += timeout_ms / 1000;
			ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
			if (ts.tv_nsec >= 1000000000L) {
				ts.tv_sec++;
				ts.tv_nsec -= 1000000000L;
			}
			int ret = pthread_cond_timedwait(&r->cond, &r->lock,
							 &ts);
			if (ret != 0) {
				pthread_mutex_unlock(&r->lock);
				return OVE_ERR_TIMEOUT;
			}
		}
	}

	/* Read length prefix. */
	uint32_t pos = r->read_pos & (RING_SIZE - 1);
	uint16_t len = r->buf[pos];
	pos = (pos + 1) & (RING_SIZE - 1);
	len |= (uint16_t)r->buf[pos] << 8;
	pos = (pos + 1) & (RING_SIZE - 1);

	if (len > buf_size) {
		/* Message too large for caller's buffer -- skip it. */
		r->read_pos += (uint32_t)sizeof(uint16_t) + len;
		pthread_mutex_unlock(&r->lock);
		*out_len = 0;
		return OVE_ERR_INVALID_PARAM;
	}

	/* Read payload. */
	uint8_t *dst = (uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		dst[i] = r->buf[pos];
		pos = (pos + 1) & (RING_SIZE - 1);
	}

	r->read_pos += (uint32_t)sizeof(uint16_t) + len;
	*out_len = len;

	pthread_mutex_unlock(&r->lock);
	return OVE_OK;
}

/* ── Direct transport private state ────────────────────────────────── */

struct direct_priv {
	struct ring event_ring; /* firmware -> WS server */
	struct ring cmd_ring;   /* WS server -> firmware */
};

/* ── Transport ops ─────────────────────────────────────────────────── */

static int direct_open(struct ove_sim_transport *t, const char *endpoint)
{
	(void)endpoint;
	struct direct_priv *p = (struct direct_priv *)t->priv;
	ring_init(&p->event_ring);
	ring_init(&p->cmd_ring);
	return OVE_OK;
}

static void direct_close(struct ove_sim_transport *t)
{
	struct direct_priv *p = (struct direct_priv *)t->priv;
	ring_destroy(&p->event_ring);
	ring_destroy(&p->cmd_ring);
}

static int direct_send_event(struct ove_sim_transport *t,
			     const struct ove_sim_event *event)
{
	struct direct_priv *p = (struct direct_priv *)t->priv;
	size_t total = sizeof(*event) + event->data_len;

	if (total > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;

	return ring_write(&p->event_ring, event, (uint16_t)total);
}

static int direct_recv_cmd(struct ove_sim_transport *t,
			   struct ove_sim_cmd *cmd, size_t cmd_size,
			   uint32_t timeout_ms)
{
	struct direct_priv *p = (struct direct_priv *)t->priv;
	uint16_t out_len = 0;
	return ring_read(&p->cmd_ring, cmd, cmd_size, &out_len, timeout_ms);
}

/* ── Host-side ops (WS server reads events, writes commands) ──────── */

static int direct_read_event(struct ove_sim_transport *t,
			     void *buf, size_t buf_size,
			     uint16_t *out_len, uint32_t timeout_ms)
{
	struct direct_priv *p = (struct direct_priv *)t->priv;
	return ring_read(&p->event_ring, buf, buf_size, out_len, timeout_ms);
}

static int direct_write_cmd(struct ove_sim_transport *t,
			    const void *data, uint16_t len)
{
	struct direct_priv *p = (struct direct_priv *)t->priv;
	return ring_write(&p->cmd_ring, data, len);
}

/* ── Display / audio ops (delegate to WS server mailbox) ──────────── */

#include "ove_sim_ws.h"

static int direct_flush_display(struct ove_sim_transport *t,
				const void *fb, size_t fb_len,
				uint16_t x1, uint16_t y1,
				uint16_t x2, uint16_t y2)
{
	(void)t;
	if (!ove_sim_ws_has_clients())
		return OVE_OK;

	size_t hdr_len = 8;
	size_t total = hdr_len + fb_len;
	uint8_t *frame = malloc(total);
	if (!frame)
		return OVE_ERR_NO_MEMORY;

	uint16_t coords[4] = {x1, y1, x2, y2};
	memcpy(frame, coords, 8);
	memcpy(frame + 8, fb, fb_len);
	ove_sim_ws_broadcast(OVE_SIM_WS_FRAME_FB, frame, total);
	free(frame);
	return OVE_OK;
}

static int direct_push_audio(struct ove_sim_transport *t,
			     const void *samples, size_t len,
			     uint32_t sample_rate, uint16_t channels,
			     uint16_t bit_depth)
{
	(void)t;
	if (!ove_sim_ws_has_clients())
		return OVE_OK;

	size_t hdr = 8;
	size_t total = hdr + len;
	uint8_t *frame = malloc(total);
	if (!frame)
		return OVE_ERR_NO_MEMORY;

	memcpy(frame, &sample_rate, 4);
	memcpy(frame + 4, &channels, 2);
	memcpy(frame + 6, &bit_depth, 2);
	memcpy(frame + 8, samples, len);
	ove_sim_ws_broadcast(OVE_SIM_WS_FRAME_AUDIO, frame, total);
	free(frame);
	return OVE_OK;
}

static size_t direct_pull_audio(struct ove_sim_transport *t, void *samples,
				size_t len)
{
	(void)t;
	/* Audio input comes via plugin command handler (sim_audio.c).
	 * Nothing to do here -- the sim_audio plugin's input ring
	 * is filled by FRAME_CMD / AUDIO_CMD_INJECT from the WS server. */
	(void)samples;
	(void)len;
	return 0;
}

static const struct ove_sim_transport_ops direct_ops = {
	.open          = direct_open,
	.close         = direct_close,
	.send_event    = direct_send_event,
	.recv_cmd      = direct_recv_cmd,
	.read_event    = direct_read_event,
	.write_cmd     = direct_write_cmd,
	.flush_display = direct_flush_display,
	.push_audio    = direct_push_audio,
	.pull_audio    = direct_pull_audio,
};

/* ── Public factory ────────────────────────────────────────────────── */

static struct direct_priv direct_priv_instance;

int ove_sim_transport_direct_create(struct ove_sim_transport *t)
{
	if (!t)
		return OVE_ERR_INVALID_PARAM;

	t->ops = &direct_ops;
	t->priv = &direct_priv_instance;
	return OVE_OK;
}

/* ── Functions for the WS server to read events / inject commands ──── */

/**
 * @brief Read the next event from the event ring (WS server calls this).
 *
 * @param[out] buf       Buffer for the event.
 * @param[in]  buf_size  Size of @p buf.
 * @param[out] out_len   Actual event size.
 * @param[in]  timeout_ms  Wait timeout.
 * @return 0 on success, OVE_ERR_TIMEOUT if none available.
 */
int ove_sim_direct_read_event(void *buf, size_t buf_size,
			      uint16_t *out_len, uint32_t timeout_ms)
{
	return ring_read(&direct_priv_instance.event_ring,
			 buf, buf_size, out_len, timeout_ms);
}

/**
 * @brief Write a command into the command ring (WS server calls this).
 *
 * @param[in] data  Command data.
 * @param[in] len   Command length.
 * @return 0 on success.
 */
int ove_sim_direct_write_cmd(const void *data, uint16_t len)
{
	return ring_write(&direct_priv_instance.cmd_ring, data, len);
}
