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
 *   /dev/shm/ove-audio — output then input (ove_sim_audio_ring layout)
 *
 * Whoever starts the emulator creates these files before the guest runs:
 * qemu-run.sh natively, sim/dashboard/qemu-wasm.js when QEMU itself runs
 * in a browser (the files are then in its in-memory filesystem).
 */

#include "ove/types.h"
#include "ove_config.h"

#ifdef CONFIG_OVE_BOARD_QEMU_MPS2_AN500

#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_shm.h"
#include "ove/thread.h"
#include "ove_sim_audio_ring.h"
#include "semihosting.h"

#include <string.h>

/* ── Framebuffer protocol (config/scripts/ove_dashboard/shm.py) ──── */

#define FB_PATH "/dev/shm/ove-fb"
#define FB_MAGIC 0x42465854 /* "TXFB" */
#define FB_FMT_RGB565 0
#define FB_FMT_XRGB8888 1

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

/* ── Private state ────────────────────────────────────────────────── */

struct shm_guest_priv {
	/* Plugin events/commands (/dev/shm/ove-sim) */
	int sim_fd;
	uint32_t event_wpos;
	uint32_t cmd_rpos;
	uint32_t event_dropped;

	/* Display (/dev/shm/ove-fb) */
	int fb_fd;
	uint16_t fb_width;
	uint16_t fb_height;

	/* Audio (/dev/shm/ove-audio) */
	int audio_fd;
	uint32_t audio_out_wpos;
	uint32_t audio_in_rpos;
	uint8_t audio_init_done;
	uint8_t audio_out_format_set;
};

/* ── Semihosting ring helpers ─────────────────────────────────────── */

static void sh_write_u32(int fd, uint32_t offset, uint32_t val)
{
	sh_seek(fd, offset);
	sh_write(fd, &val, sizeof(val));
}

static uint32_t sh_read_u32(int fd, uint32_t offset)
{
	uint32_t val = 0;
	sh_seek(fd, offset);
	sh_read(fd, &val, sizeof(val));
	return val;
}

static void sh_ring_write(int fd, uint32_t ring_off, uint32_t ring_size, uint32_t wpos,
			  const void *data, uint32_t len)
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

static uint32_t sh_ring_read(int fd, uint32_t ring_off, uint32_t ring_size, uint32_t rpos,
			     void *data, uint32_t len)
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
	p->sim_fd = sh_open(SIM_SHM_PATH, 3); /* r+b (open existing) */
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
		p->event_dropped = 0;
	}

	/* Open framebuffer SHM. */
	p->fb_fd = sh_open(FB_PATH, 3); /* r+b (open existing) */

	/* Audio SHM opened lazily in push/pull. */
	p->audio_fd = -1;
	p->audio_init_done = 0;

	return OVE_OK;
}

static void guest_close(struct ove_sim_transport *t)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd >= 0) {
		sh_close(p->sim_fd);
		p->sim_fd = -1;
	}
	if (p->fb_fd >= 0) {
		sh_close(p->fb_fd);
		p->fb_fd = -1;
	}
	if (p->audio_fd >= 0) {
		sh_close(p->audio_fd);
		p->audio_fd = -1;
	}
}

/* The hosts (config/scripts/ove_dashboard/shm.py, sim/dashboard/qemu-wasm.js)
 * hard-code these offsets. */
_Static_assert(sizeof(struct sim_shm_header) == SIM_SHM_HDR_SIZE, "shm header size");
_Static_assert(offsetof(struct sim_shm_header, event_write_pos) == 16, "event_write_pos offset");
_Static_assert(offsetof(struct sim_shm_header, event_read_pos) == 20, "event_read_pos offset");
_Static_assert(offsetof(struct sim_shm_header, event_dropped) == 32, "event_dropped offset");

static int guest_send_event(struct ove_sim_transport *t, const struct ove_sim_event *event)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd < 0)
		return OVE_OK; /* headless */

	size_t total = sizeof(*event) + event->data_len;
	if (total > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;

	uint16_t len = (uint16_t)total;

	/*
	 * Backpressure: never lap the host's read position. If the event
	 * doesn't fit in the free space, drop it, count it and tell the
	 * caller, rather than overwriting unread batches (which used to
	 * corrupt the host's view and silently lose the drop counts the
	 * batches carry).  The caller folds what the lost event carried back
	 * into its own drop counter, so a later event reports it.
	 */
	uint32_t rpos = sh_read_u32(p->sim_fd, offsetof(struct sim_shm_header, event_read_pos));
	uint32_t used = p->event_wpos - rpos;
	if (used > SIM_SHM_RING_SIZE)
		used = SIM_SHM_RING_SIZE; /* already lapped; treat as full */
	if ((uint32_t)total + 2u > SIM_SHM_RING_SIZE - used) {
		p->event_dropped++;
		sh_write_u32(p->sim_fd, offsetof(struct sim_shm_header, event_dropped),
			     p->event_dropped);
		return OVE_ERR_QUEUE_FULL;
	}

	/* Write length prefix + event into event ring. */
	uint8_t lenbuf[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
	sh_ring_write(p->sim_fd, SIM_SHM_EVENT_RING_OFF, SIM_SHM_RING_SIZE, p->event_wpos, lenbuf,
		      2);
	p->event_wpos += 2;
	sh_ring_write(p->sim_fd, SIM_SHM_EVENT_RING_OFF, SIM_SHM_RING_SIZE, p->event_wpos, event,
		      (uint32_t)total);
	p->event_wpos += (uint32_t)total;

	/* Update write position in header. */
	sh_write_u32(p->sim_fd, offsetof(struct sim_shm_header, event_write_pos), p->event_wpos);
	return OVE_OK;
}

static int guest_recv_cmd(struct ove_sim_transport *t, struct ove_sim_cmd *cmd, size_t cmd_size,
			  uint32_t timeout_ms)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->sim_fd < 0)
		return OVE_ERR_TIMEOUT;

	uint32_t wpos = sh_read_u32(p->sim_fd, offsetof(struct sim_shm_header, cmd_write_pos));
	uint32_t avail = wpos - p->cmd_rpos;
	if (avail < 2) {
		(void)timeout_ms; /* no blocking in semihosting mode */
		return OVE_ERR_TIMEOUT;
	}

	/* Read length prefix. */
	uint8_t lenbuf[2];
	sh_ring_read(p->sim_fd, SIM_SHM_CMD_RING_OFF, SIM_SHM_RING_SIZE, p->cmd_rpos, lenbuf, 2);
	uint16_t len = (uint16_t)lenbuf[0] | ((uint16_t)lenbuf[1] << 8);
	p->cmd_rpos += 2;

	if (len > cmd_size) {
		p->cmd_rpos += len; /* skip */
		sh_write_u32(p->sim_fd, offsetof(struct sim_shm_header, cmd_read_pos), p->cmd_rpos);
		return OVE_ERR_INVALID_PARAM;
	}

	sh_ring_read(p->sim_fd, SIM_SHM_CMD_RING_OFF, SIM_SHM_RING_SIZE, p->cmd_rpos, cmd, len);
	p->cmd_rpos += len;

	sh_write_u32(p->sim_fd, offsetof(struct sim_shm_header, cmd_read_pos), p->cmd_rpos);
	return OVE_OK;
}

/* ── Display flush (semihosting write to /dev/shm/ove-fb) ─────────── */

static int guest_flush_display(struct ove_sim_transport *t, const void *fb, size_t fb_len,
			       uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	if (p->fb_fd < 0)
		return OVE_OK; /* headless */

	uint16_t w = x2 - x1 + 1;
	uint16_t h = y2 - y1 + 1;

	/* Write header + pixels atomically.
	 * For simplicity, always write full frame from (0,0). */
	struct fb_header hdr = {
		.magic = FB_MAGIC,
		.width = w,
		.height = h,
		.format = FB_FMT_XRGB8888,
		.dirty = 1,
	};
	sh_seek(p->fb_fd, 0);
	sh_write(p->fb_fd, &hdr, sizeof(hdr));
	sh_write(p->fb_fd, fb, (uint32_t)fb_len);

	p->fb_width = w;
	p->fb_height = h;
	return OVE_OK;
}

/* ── Audio push/pull (semihosting to /dev/shm/ove-audio) ──────────── */

static void guest_audio_init(struct shm_guest_priv *p)
{
	if (p->audio_init_done)
		return;

	p->audio_fd = sh_open("/dev/shm/ove-audio", 7); /* w+b (truncates) */
	if (p->audio_fd < 0)
		return;

	uint8_t hdr[OVE_RING_OFF_BUF] = {0};
	uint32_t ring_size = OVE_SIM_AUDIO_RING_SIZE;
	memcpy(hdr + OVE_RING_OFF_SIZE, &ring_size, sizeof(ring_size));
	sh_seek(p->audio_fd, 0);
	sh_write(p->audio_fd, hdr, sizeof(hdr));

	/* Preserve any input the host has already queued. */
	const uint32_t input = sizeof(struct ove_sim_audio_ring);
	sh_write_u32(p->audio_fd, input + OVE_RING_OFF_SIZE, ring_size);
	p->audio_in_rpos = sh_read_u32(p->audio_fd, input + OVE_RING_OFF_READ_POS);
	p->audio_out_wpos = 0;
	p->audio_out_format_set = 0;
	p->audio_init_done = 1;
}

static int guest_push_audio(struct ove_sim_transport *t, const void *samples, size_t len,
			    uint32_t sample_rate, uint16_t channels, uint16_t bit_depth)
{
	/* Playback may lap the host. Limit each push to one ring. */
	if (len > OVE_SIM_AUDIO_RING_SIZE)
		return OVE_OK;

	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	guest_audio_init(p);
	if (p->audio_fd < 0)
		return OVE_OK;

	if (!p->audio_out_format_set) {
		sh_seek(p->audio_fd, OVE_RING_OFF_SAMPLE_RATE);
		if (sh_write(p->audio_fd, &sample_rate, sizeof(sample_rate)))
			return OVE_OK;
		sh_seek(p->audio_fd, OVE_RING_OFF_BIT_DEPTH);
		if (sh_write(p->audio_fd, &bit_depth, sizeof(bit_depth)))
			return OVE_OK;
		/* Publish channels last for the host's format check. */
		sh_seek(p->audio_fd, OVE_RING_OFF_CHANNELS);
		if (sh_write(p->audio_fd, &channels, sizeof(channels)))
			return OVE_OK;
		p->audio_out_format_set = 1;
	}

	/* Write data to ring, checking semihosting errors. */
	uint32_t count = (uint32_t)len;
	uint32_t mask = OVE_SIM_AUDIO_RING_SIZE - 1;
	uint32_t pos = p->audio_out_wpos & mask;
	uint32_t first = OVE_SIM_AUDIO_RING_SIZE - pos;
	const uint8_t *src = (const uint8_t *)samples;

	if (first >= count) {
		sh_seek(p->audio_fd, OVE_RING_OFF_BUF + pos);
		int err = sh_write(p->audio_fd, src, count);
		if (err)
			return OVE_OK; /* semihosting failed */
	} else {
		sh_seek(p->audio_fd, OVE_RING_OFF_BUF + pos);
		int err1 = sh_write(p->audio_fd, src, first);
		sh_seek(p->audio_fd, OVE_RING_OFF_BUF);
		int err2 = sh_write(p->audio_fd, src + first, count - first);
		if (err1 || err2)
			return OVE_OK;
	}

	p->audio_out_wpos += count;
	sh_write_u32(p->audio_fd, OVE_RING_OFF_WRITE_POS, p->audio_out_wpos);
	return OVE_OK;
}

static size_t guest_pull_audio(struct ove_sim_transport *t, void *samples, size_t len)
{
	struct shm_guest_priv *p = (struct shm_guest_priv *)t->priv;
	guest_audio_init(p);
	if (p->audio_fd < 0)
		return 0;

	const uint32_t input = sizeof(struct ove_sim_audio_ring);
	uint32_t wpos = sh_read_u32(p->audio_fd, input + OVE_RING_OFF_WRITE_POS);
	uint32_t avail = wpos - p->audio_in_rpos;
	if (avail == 0 || avail > OVE_SIM_AUDIO_RING_SIZE)
		return 0;

	uint32_t to_read = len < avail ? (uint32_t)len : avail;
	sh_ring_read(p->audio_fd, input + OVE_RING_OFF_BUF, OVE_SIM_AUDIO_RING_SIZE,
		     p->audio_in_rpos, samples, to_read);
	p->audio_in_rpos += to_read;

	sh_write_u32(p->audio_fd, input + OVE_RING_OFF_READ_POS, p->audio_in_rpos);
	return to_read;
}

/* ── Vtable ───────────────────────────────────────────────────────── */

static const struct ove_sim_transport_ops shm_guest_ops = {
	.open = guest_open,
	.close = guest_close,
	.send_event = guest_send_event,
	.recv_cmd = guest_recv_cmd,
	.flush_display = guest_flush_display,
	.push_audio = guest_push_audio,
	.pull_audio = guest_pull_audio,
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
