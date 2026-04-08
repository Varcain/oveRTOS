/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Guest-side shared-memory transport for QEMU simulation mode.
 *
 * Runs inside the QEMU ARM guest firmware.  Communicates with the host
 * via ARM semihosting file I/O on three shared-memory files:
 *
 *   /dev/shm/ove-sim   — plugin events/commands (ove_sim_shm.h layout)
 *   /dev/shm/ove-fb    — XRGB8888 framebuffer (fb_header + pixels)
 *   /dev/shm/ove-audio — PCM ring buffers (audio_shm_header + 2 rings)
 *
 * These files are pre-created by qemu-run.sh before the guest starts.
 */

#include "ove/types.h"

#ifdef CONFIG_OVE_BOARD_QEMU_MPS2_AN500

#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_shm.h"
#include "ove/thread.h"
#include "semihosting.h"

#include <string.h>

/* ── Framebuffer protocol (matches ove-dashboard-bridge.py) ──────── */

#define FB_PATH       "/dev/shm/ove-fb"
#define FB_MAGIC      0x42465854  /* "TXFB" */
#define FB_FMT_RGB565   0
#define FB_FMT_XRGB8888 1

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

/* ── Audio protocol (matches qemu_audio_shm.h) ───────────────────── */

#define AUDIO_PATH       "/dev/shm/ove-audio"
#define AUDIO_MAGIC      0x4F564155  /* "OVAU" */
#define AUDIO_HDR_SIZE   64
#define AUDIO_RING_SIZE  (1u << 17)  /* 128 KB per direction */
#define AUDIO_OUT_RING   AUDIO_HDR_SIZE
#define AUDIO_IN_RING    (AUDIO_HDR_SIZE + AUDIO_RING_SIZE)

/* Offsets into audio header. */
#define AUDIO_OFF_MAGIC       0
#define AUDIO_OFF_SAMPLE_RATE 4
#define AUDIO_OFF_CHANNELS    8
#define AUDIO_OFF_BIT_DEPTH   10
#define AUDIO_OFF_RING_SIZE   16
#define AUDIO_OFF_OUT_WPOS    20
#define AUDIO_OFF_OUT_RPOS    24
#define AUDIO_OFF_IN_WPOS     28
#define AUDIO_OFF_IN_RPOS     32

/* ── Private state ────────────────────────────────────────────────── */

struct shm_guest_priv {
	/* Plugin events/commands (/dev/shm/ove-sim) */
	int      sim_fd;
	uint32_t event_wpos;
	uint32_t cmd_rpos;

	/* Display (/dev/shm/ove-fb) */
	int      fb_fd;
	uint16_t fb_width;
	uint16_t fb_height;

	/* Audio (/dev/shm/ove-audio) */
	int      audio_fd;
	uint32_t audio_out_wpos;
	uint32_t audio_in_rpos;
	uint8_t  audio_init_done;
};

/* ── Semihosting ring helpers ─────────────────────────────────────── */

static void sh_write_u32(int fd, uint32_t offset, uint32_t val)
{
	sh_seek(fd, offset);
	sh_write(fd, &val, sizeof(val));
}

static uint32_t sh_read_u32(int fd, uint32_t offset)
{
	uint32_t val;
	sh_seek(fd, offset);
	sh_read(fd, &val, sizeof(val));
	return val;
}

static void sh_ring_write(int fd, uint32_t ring_off, uint32_t ring_size,
			   uint32_t wpos, const void *data, uint32_t len)
{
	uint32_t mask = ring_size - 1;
	uint32_t pos = wpos & mask;
	uint32_t first = ring_size - pos;
	const uint8_t *src = (const uint8_t *)data;

	if (first >= len) {
		sh_seek(fd, ring_off + pos);
		sh_write(fd, src, len);
	} else {
		sh_seek(fd, ring_off + pos);
		sh_write(fd, src, first);
		sh_seek(fd, ring_off);
		sh_write(fd, src + first, len - first);
	}
}

static uint32_t sh_ring_read(int fd, uint32_t ring_off, uint32_t ring_size,
			      uint32_t rpos, void *data, uint32_t len)
{
	uint32_t mask = ring_size - 1;
	uint32_t pos = rpos & mask;
	uint32_t first = ring_size - pos;
	uint8_t *dst = (uint8_t *)data;

	if (first >= len) {
		sh_seek(fd, ring_off + pos);
		sh_read(fd, dst, len);
	} else {
		sh_seek(fd, ring_off + pos);
		sh_read(fd, dst, first);
		sh_seek(fd, ring_off);
		sh_read(fd, dst + first, len - first);
	}
	return len;
}

/* ── Transport ops ────────────────────────────────────────────────── */

static int guest_open(struct ove_sim_transport *t, const char *endpoint)
{
	(void)endpoint;
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;

	/* Open plugin event/command SHM. */
	p->sim_fd = sh_open(SIM_SHM_PATH, 7); /* r+b */
	if (p->sim_fd >= 0) {
		/* Write header. */
		struct sim_shm_header hdr;
		memset(&hdr, 0, sizeof(hdr));
		hdr.magic = SIM_SHM_MAGIC;
		hdr.version = SIM_SHM_VERSION;
		hdr.ring_size = SIM_SHM_RING_SIZE;
		sh_seek(p->sim_fd, 0);
		sh_write(p->sim_fd, &hdr, sizeof(hdr));
		p->event_wpos = 0;
		p->cmd_rpos = 0;
	}

	/* Open framebuffer SHM. */
	p->fb_fd = sh_open(FB_PATH, 7); /* r+b */

	/* Audio SHM opened lazily in push/pull. */
	p->audio_fd = -1;
	p->audio_init_done = 0;

	return OVE_OK;
}

static void guest_close(struct ove_sim_transport *t)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd >= 0)   { sh_close(p->sim_fd);   p->sim_fd = -1; }
	if (p->fb_fd >= 0)    { sh_close(p->fb_fd);    p->fb_fd = -1; }
	if (p->audio_fd >= 0) { sh_close(p->audio_fd); p->audio_fd = -1; }
}

static int guest_send_event(struct ove_sim_transport *t,
			     const struct ove_sim_event *event)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd < 0)
		return OVE_OK; /* headless */

	size_t total = sizeof(*event) + event->data_len;
	if (total > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;

	uint16_t len = (uint16_t)total;

	/* Write length prefix + event into event ring. */
	uint8_t lenbuf[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
	sh_ring_write(p->sim_fd, SIM_SHM_EVENT_RING_OFF, SIM_SHM_RING_SIZE,
		      p->event_wpos, lenbuf, 2);
	p->event_wpos += 2;
	sh_ring_write(p->sim_fd, SIM_SHM_EVENT_RING_OFF, SIM_SHM_RING_SIZE,
		      p->event_wpos, event, (uint32_t)total);
	p->event_wpos += (uint32_t)total;

	/* Update write position in header. */
	sh_write_u32(p->sim_fd,
		     offsetof(struct sim_shm_header, event_write_pos),
		     p->event_wpos);
	return OVE_OK;
}

static int guest_recv_cmd(struct ove_sim_transport *t,
			   struct ove_sim_cmd *cmd, size_t cmd_size,
			   uint32_t timeout_ms)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd < 0)
		return OVE_ERR_TIMEOUT;

	uint32_t wpos = sh_read_u32(p->sim_fd,
				    offsetof(struct sim_shm_header,
					     cmd_write_pos));
	uint32_t avail = wpos - p->cmd_rpos;
	if (avail < 2) {
		(void)timeout_ms; /* no blocking in semihosting mode */
		return OVE_ERR_TIMEOUT;
	}

	/* Read length prefix. */
	uint8_t lenbuf[2];
	sh_ring_read(p->sim_fd, SIM_SHM_CMD_RING_OFF, SIM_SHM_RING_SIZE,
		     p->cmd_rpos, lenbuf, 2);
	uint16_t len = (uint16_t)lenbuf[0] | ((uint16_t)lenbuf[1] << 8);
	p->cmd_rpos += 2;

	if (len > cmd_size) {
		p->cmd_rpos += len; /* skip */
		sh_write_u32(p->sim_fd,
			     offsetof(struct sim_shm_header, cmd_read_pos),
			     p->cmd_rpos);
		return OVE_ERR_INVALID_PARAM;
	}

	sh_ring_read(p->sim_fd, SIM_SHM_CMD_RING_OFF, SIM_SHM_RING_SIZE,
		     p->cmd_rpos, cmd, len);
	p->cmd_rpos += len;

	sh_write_u32(p->sim_fd,
		     offsetof(struct sim_shm_header, cmd_read_pos),
		     p->cmd_rpos);
	return OVE_OK;
}

/* ── Display flush (semihosting write to /dev/shm/ove-fb) ─────────── */

static int guest_flush_display(struct ove_sim_transport *t,
			       const void *fb, size_t fb_len,
			       uint16_t x1, uint16_t y1,
			       uint16_t x2, uint16_t y2)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->fb_fd < 0)
		return OVE_OK; /* headless */

	uint16_t w = x2 - x1 + 1;
	uint16_t h = y2 - y1 + 1;

	/* Write header + pixels atomically.
	 * For simplicity, always write full frame from (0,0). */
	struct fb_header hdr = {
		.magic  = FB_MAGIC,
		.width  = w,
		.height = h,
		.format = FB_FMT_XRGB8888,
		.dirty  = 1,
	};
	sh_seek(p->fb_fd, 0);
	sh_write(p->fb_fd, &hdr, sizeof(hdr));
	sh_write(p->fb_fd, fb, (uint32_t)fb_len);

	p->fb_width = w;
	p->fb_height = h;
	return OVE_OK;
}

/* ── Audio push/pull (semihosting to /dev/shm/ove-audio) ──────────── */

static void guest_audio_init(struct shm_guest_priv *p,
			     uint32_t sr, uint16_t ch, uint16_t bd)
{
	if (p->audio_init_done)
		return;

	p->audio_fd = sh_open(AUDIO_PATH, 7); /* r+b */
	if (p->audio_fd < 0)
		return;

	/* Write header. */
	uint8_t hdr[AUDIO_HDR_SIZE];
	memset(hdr, 0, sizeof(hdr));
	uint32_t magic = AUDIO_MAGIC;
	uint32_t ring_size = AUDIO_RING_SIZE;
	memcpy(hdr + AUDIO_OFF_MAGIC, &magic, 4);
	memcpy(hdr + AUDIO_OFF_SAMPLE_RATE, &sr, 4);
	memcpy(hdr + AUDIO_OFF_CHANNELS, &ch, 2);
	memcpy(hdr + AUDIO_OFF_BIT_DEPTH, &bd, 2);
	memcpy(hdr + AUDIO_OFF_RING_SIZE, &ring_size, 4);
	sh_seek(p->audio_fd, 0);
	sh_write(p->audio_fd, hdr, AUDIO_HDR_SIZE);

	p->audio_out_wpos = 0;
	p->audio_in_rpos = 0;
	p->audio_init_done = 1;
}

static int guest_push_audio(struct ove_sim_transport *t,
			    const void *samples, size_t len,
			    uint32_t sample_rate, uint16_t channels,
			    uint16_t bit_depth)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	guest_audio_init(p, sample_rate, channels, bit_depth);
	if (p->audio_fd < 0)
		return OVE_OK;

	/* Write data to ring, checking semihosting errors. */
	uint32_t mask = AUDIO_RING_SIZE - 1;
	uint32_t pos = p->audio_out_wpos & mask;
	uint32_t first = AUDIO_RING_SIZE - pos;
	const uint8_t *src = (const uint8_t *)samples;

	if (first >= (uint32_t)len) {
		sh_seek(p->audio_fd, AUDIO_OUT_RING + pos);
		int err = sh_write(p->audio_fd, src, (uint32_t)len);
		if (err)
			return OVE_OK; /* semihosting failed */
	} else {
		sh_seek(p->audio_fd, AUDIO_OUT_RING + pos);
		int err1 = sh_write(p->audio_fd, src, first);
		sh_seek(p->audio_fd, AUDIO_OUT_RING);
		int err2 = sh_write(p->audio_fd, src + first,
				     (uint32_t)len - first);
		if (err1 || err2)
			return OVE_OK;
	}

	p->audio_out_wpos += (uint32_t)len;
	sh_write_u32(p->audio_fd, AUDIO_OFF_OUT_WPOS, p->audio_out_wpos);
	return OVE_OK;
}

static size_t guest_pull_audio(struct ove_sim_transport *t,
			       void *samples, size_t len)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	/* Open audio fd if not yet done (source may run before sink). */
	if (p->audio_fd < 0 && !p->audio_init_done)
		guest_audio_init(p, 16000, 1, 16);
	if (p->audio_fd < 0)
		return 0;

	uint32_t wpos = sh_read_u32(p->audio_fd, AUDIO_OFF_IN_WPOS);
	uint32_t avail = wpos - p->audio_in_rpos;
	if (avail > AUDIO_RING_SIZE)
		avail = 0;
	if (avail == 0)
		return 0;

	uint32_t to_read = avail < (uint32_t)len ? avail : (uint32_t)len;
	sh_ring_read(p->audio_fd, AUDIO_IN_RING, AUDIO_RING_SIZE,
		     p->audio_in_rpos, samples, to_read);
	p->audio_in_rpos += to_read;

	sh_write_u32(p->audio_fd, AUDIO_OFF_IN_RPOS, p->audio_in_rpos);
	return to_read;
}

/* ── Vtable ───────────────────────────────────────────────────────── */

static const struct ove_sim_transport_ops shm_guest_ops = {
	.open          = guest_open,
	.close         = guest_close,
	.send_event    = guest_send_event,
	.recv_cmd      = guest_recv_cmd,
	.flush_display = guest_flush_display,
	.push_audio    = guest_push_audio,
	.pull_audio    = guest_pull_audio,
	/* read_event/write_cmd are host-side only — NULL here. */
};

/* ── Public factory ───────────────────────────────────────────────── */

static struct shm_guest_priv guest_priv_instance;

int ove_sim_transport_shm_guest_create(struct ove_sim_transport *t)
{
	if (!t)
		return OVE_ERR_INVALID_PARAM;

	memset(&guest_priv_instance, 0, sizeof(guest_priv_instance));
	guest_priv_instance.sim_fd = -1;
	guest_priv_instance.fb_fd = -1;
	guest_priv_instance.audio_fd = -1;

	t->ops = &shm_guest_ops;
	t->priv = &guest_priv_instance;
	return OVE_OK;
}

#endif /* CONFIG_OVE_BOARD_QEMU_MPS2_AN500 */
