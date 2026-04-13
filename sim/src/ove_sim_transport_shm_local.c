/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Local shared-memory transport for host POSIX simulation.
 *
 * Same shmem protocol as the QEMU guest transport, but uses mmap
 * instead of ARM semihosting.  The external dashboard bridge
 * (ove-dashboard-bridge.py) reads the shmem and serves the browser.
 *
 *   /dev/shm/ove-sim   — plugin events/commands (ove_sim_shm.h)
 *   /dev/shm/ove-fb    — XRGB8888 framebuffer
 *   /dev/shm/ove-audio — PCM ring buffers
 */

#include "ove/types.h"
#include "ove_config.h"

#if !defined(__EMSCRIPTEN__) && !defined(CONFIG_OVE_BOARD_QEMU_MPS2_AN500)

#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_shm.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Framebuffer protocol (shared with QEMU guest transport) ─────── */

#define FB_PATH         "/dev/shm/ove-fb"
#define FB_MAGIC        0x42465854  /* "TXFB" */
#define FB_FMT_XRGB8888 1
#define FB_MAX_SIZE     (16 + 1920 * 1080 * 4)

struct fb_header {
	uint32_t magic;
	uint16_t width;
	uint16_t height;
	uint32_t format;
	uint32_t dirty;
};

/* ── Audio protocol ──────────────────────────────────────────────── */
/*
 * SHM layout: two ove_sim_audio_ring structs back-to-back.
 *   [output ring: 32-byte header + OVE_SIM_AUDIO_RING_SIZE data]
 *   [input ring:  32-byte header + OVE_SIM_AUDIO_RING_SIZE data]
 * Matches ove_sim_audio_ring.h and ove-dashboard-bridge.py.
 */

#include "ove_sim_audio_ring.h"

#define AUDIO_PATH      "/dev/shm/ove-audio"
#define AUDIO_RING_TOTAL (sizeof(struct ove_sim_audio_ring))
#define AUDIO_SHM_TOTAL  (2u * AUDIO_RING_TOTAL)

/* ── Pointer input (bridge → firmware) ───────────────────────────── */
/*
 * /dev/shm/ove-input — 16-byte struct the dashboard bridge writes to
 * on every mouse/touch event. The firmware side lazily mmaps it from
 * ove_sim_input_get() so clicks routed through the browser dashboard
 * actually reach LVGL's pointer indev.
 *
 * Layout (little-endian):
 *   offset  0: uint32_t magic    ("INPT" = 0x54504E49)
 *   offset  4: int16_t  x
 *   offset  6: int16_t  y
 *   offset  8: uint8_t  pressed
 *   offset  9: uint8_t  reserved[7]
 */
#define INPUT_PATH      "/dev/shm/ove-input"
#define INPUT_MAGIC     0x54504E49u  /* "INPT" */
#define INPUT_SHM_SIZE  16

/* ── Private state ───────────────────────────────────────────────── */

struct shm_local_priv {
	/* Plugin events/commands (/dev/shm/ove-sim) */
	int      sim_fd;
	uint8_t *sim_base;
	uint32_t event_wpos;
	uint32_t cmd_rpos;

	/* Display (/dev/shm/ove-fb) */
	int      fb_fd;
	uint8_t *fb_base;
	size_t   fb_map_size;

	/* Audio (/dev/shm/ove-audio) — two ove_sim_audio_ring structs */
	int                        audio_fd;
	struct ove_sim_audio_ring *audio_out;  /* playback: firmware → bridge */
	struct ove_sim_audio_ring *audio_in;   /* capture:  bridge → firmware */
	uint8_t                    audio_init_done;

	/* Pointer input (/dev/shm/ove-input) */
	int      input_fd;
	uint8_t *input_base;
};

/* ── mmap helpers ────────────────────────────────────────────────── */

static int shm_create(const char *path, size_t size, int *fd_out,
		       uint8_t **base_out)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	uint8_t *base = mmap(NULL, size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		close(fd);
		unlink(path);
		return -1;
	}
	memset(base, 0, size);
	*fd_out = fd;
	*base_out = base;
	return 0;
}

static void shm_destroy(const char *path, int fd, uint8_t *base, size_t size)
{
	if (base && base != MAP_FAILED)
		munmap(base, size);
	if (fd >= 0)
		close(fd);
	if (path)
		unlink(path);
}

/* ── Ring helpers (direct memory, no semihosting) ────────────────── */

static inline void ring_write(uint8_t *ring, uint32_t ring_size,
			       uint32_t wpos, const void *data, uint32_t len)
{
	uint32_t mask = ring_size - 1;
	uint32_t pos = wpos & mask;
	uint32_t first = ring_size - pos;
	const uint8_t *src = (const uint8_t *)data;

	if (first >= len) {
		memcpy(ring + pos, src, len);
	} else {
		memcpy(ring + pos, src, first);
		memcpy(ring, src + first, len - first);
	}
}

static inline uint32_t ring_read(uint8_t *ring, uint32_t ring_size,
				  uint32_t rpos, void *data, uint32_t len)
{
	uint32_t mask = ring_size - 1;
	uint32_t pos = rpos & mask;
	uint32_t first = ring_size - pos;
	uint8_t *dst = (uint8_t *)data;

	if (first >= len) {
		memcpy(dst, ring + pos, len);
	} else {
		memcpy(dst, ring + pos, first);
		memcpy(dst + first, ring, len - first);
	}
	return len;
}

/* ── Transport ops ───────────────────────────────────────────────── */

static int local_open(struct ove_sim_transport *t, const char *endpoint)
{
	(void)endpoint;
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;

	/* Create /dev/shm/ove-sim for events & commands. */
	if (shm_create(SIM_SHM_PATH, SIM_SHM_TOTAL_SIZE,
		       &p->sim_fd, &p->sim_base) == 0) {
		struct sim_shm_header *hdr =
			(struct sim_shm_header *)p->sim_base;
		hdr->magic = SIM_SHM_MAGIC;
		hdr->version = SIM_SHM_VERSION;
		hdr->ring_size = SIM_SHM_RING_SIZE;
		p->event_wpos = 0;
		p->cmd_rpos = 0;
	}

	/* Create /dev/shm/ove-fb for display. */
	if (shm_create(FB_PATH, FB_MAX_SIZE, &p->fb_fd, &p->fb_base) < 0)
		p->fb_base = NULL; /* headless — no display */

	/* Audio created lazily on first push/pull. */
	p->audio_fd = -1;
	p->audio_out = NULL;
	p->audio_in = NULL;
	p->audio_init_done = 0;

	/* Create /dev/shm/ove-input for pointer input from the dashboard
	 * bridge. Stamp the magic so the bridge (and the firmware's
	 * ove_sim_input_get() mmap path) can distinguish an initialised
	 * region from a stale leftover. */
	if (shm_create(INPUT_PATH, INPUT_SHM_SIZE,
		       &p->input_fd, &p->input_base) == 0) {
		uint32_t magic = INPUT_MAGIC;
		memcpy(p->input_base, &magic, sizeof(magic));
	} else {
		p->input_base = NULL;
		p->input_fd = -1;
	}

	return OVE_OK;
}

static void local_close(struct ove_sim_transport *t)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;

	shm_destroy(SIM_SHM_PATH, p->sim_fd, p->sim_base,
		    SIM_SHM_TOTAL_SIZE);
	p->sim_fd = -1; p->sim_base = NULL;

	shm_destroy(FB_PATH, p->fb_fd, p->fb_base, p->fb_map_size);
	p->fb_fd = -1; p->fb_base = NULL;

	if (p->audio_out)
		shm_destroy(AUDIO_PATH, p->audio_fd,
			    (uint8_t *)p->audio_out, AUDIO_SHM_TOTAL);
	p->audio_fd = -1; p->audio_out = NULL; p->audio_in = NULL;

	if (p->input_base)
		shm_destroy(INPUT_PATH, p->input_fd, p->input_base,
			    INPUT_SHM_SIZE);
	p->input_fd = -1; p->input_base = NULL;
}

/* ── Events / commands (plugin IPC) ──────────────────────────────── */

static int local_send_event(struct ove_sim_transport *t,
			     const struct ove_sim_event *event)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;
	if (!p->sim_base)
		return OVE_OK;

	size_t total = sizeof(*event) + event->data_len;
	if (total > UINT16_MAX)
		return OVE_ERR_INVALID_PARAM;

	uint8_t *ering = p->sim_base + SIM_SHM_EVENT_RING_OFF;
	uint16_t len = (uint16_t)total;
	uint8_t lenbuf[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };

	ring_write(ering, SIM_SHM_RING_SIZE, p->event_wpos, lenbuf, 2);
	p->event_wpos += 2;
	ring_write(ering, SIM_SHM_RING_SIZE, p->event_wpos,
		   event, (uint32_t)total);
	p->event_wpos += (uint32_t)total;

	struct sim_shm_header *hdr = (struct sim_shm_header *)p->sim_base;
	__atomic_store_n(&hdr->event_write_pos, p->event_wpos,
			 __ATOMIC_RELEASE);
	return OVE_OK;
}

static int local_recv_cmd(struct ove_sim_transport *t,
			   struct ove_sim_cmd *cmd, size_t cmd_size,
			   uint32_t timeout_ms)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;
	if (!p->sim_base)
		return OVE_ERR_TIMEOUT;

	struct sim_shm_header *hdr = (struct sim_shm_header *)p->sim_base;
	uint32_t wpos = __atomic_load_n(&hdr->cmd_write_pos,
					__ATOMIC_ACQUIRE);
	uint32_t avail = wpos - p->cmd_rpos;
	if (avail < 2) {
		if (timeout_ms == 0)
			return OVE_ERR_TIMEOUT;
		/* Simple poll for non-zero timeout. */
		for (uint32_t elapsed = 0; elapsed < timeout_ms; elapsed++) {
			usleep(1000);
			wpos = __atomic_load_n(&hdr->cmd_write_pos,
					       __ATOMIC_ACQUIRE);
			avail = wpos - p->cmd_rpos;
			if (avail >= 2)
				break;
		}
		if (avail < 2)
			return OVE_ERR_TIMEOUT;
	}

	uint8_t *cring = p->sim_base + SIM_SHM_CMD_RING_OFF;
	uint8_t lenbuf[2];
	ring_read(cring, SIM_SHM_RING_SIZE, p->cmd_rpos, lenbuf, 2);
	uint16_t len = (uint16_t)lenbuf[0] | ((uint16_t)lenbuf[1] << 8);
	p->cmd_rpos += 2;

	if (len > cmd_size) {
		p->cmd_rpos += len;
		__atomic_store_n(&hdr->cmd_read_pos, p->cmd_rpos,
				 __ATOMIC_RELEASE);
		return OVE_ERR_INVALID_PARAM;
	}

	ring_read(cring, SIM_SHM_RING_SIZE, p->cmd_rpos, cmd, len);
	p->cmd_rpos += len;

	__atomic_store_n(&hdr->cmd_read_pos, p->cmd_rpos, __ATOMIC_RELEASE);
	return OVE_OK;
}

/* ── Display flush ───────────────────────────────────────────────── */

static int local_flush_display(struct ove_sim_transport *t,
				const void *fb, size_t fb_len,
				uint16_t x1, uint16_t y1,
				uint16_t x2, uint16_t y2)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;
	if (!p->fb_base)
		return OVE_OK;

	uint16_t w = x2 - x1 + 1;
	uint16_t h = y2 - y1 + 1;

	struct fb_header *hdr = (struct fb_header *)p->fb_base;
	hdr->magic  = FB_MAGIC;
	hdr->width  = w;
	hdr->height = h;
	hdr->format = FB_FMT_XRGB8888;
	memcpy(p->fb_base + sizeof(struct fb_header), fb, fb_len);
	/* dirty must be written last (acts as release fence). */
	__atomic_store_n(&hdr->dirty, 1, __ATOMIC_RELEASE);
	return OVE_OK;
}

/* ── Audio push / pull ───────────────────────────────────────────── */

static void local_audio_init(struct shm_local_priv *p,
			      uint32_t sr, uint16_t ch, uint16_t bd)
{
	if (p->audio_init_done)
		return;

	uint8_t *base;
	if (shm_create(AUDIO_PATH, AUDIO_SHM_TOTAL,
		       &p->audio_fd, &base) < 0)
		return;

	p->audio_out = (struct ove_sim_audio_ring *)base;
	p->audio_in  = (struct ove_sim_audio_ring *)(base + AUDIO_RING_TOTAL);

	ove_sim_audio_ring_init(p->audio_out, sr, ch, bd);
	ove_sim_audio_ring_init(p->audio_in, sr, ch, bd);

	p->audio_init_done = 1;
}

static int local_push_audio(struct ove_sim_transport *t,
			     const void *samples, size_t len,
			     uint32_t sample_rate, uint16_t channels,
			     uint16_t bit_depth)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;
	local_audio_init(p, sample_rate, channels, bit_depth);
	if (!p->audio_out)
		return OVE_OK;

	struct ove_sim_audio_ring *r = p->audio_out;
	uint32_t mask = ove_sim_ring_mask(r);
	uint32_t wp = r->write_pos;
	uint32_t pos = wp & mask;
	uint32_t first = r->size - pos;
	const uint8_t *src = (const uint8_t *)samples;

	if (first >= (uint32_t)len) {
		memcpy(r->buf + pos, src, len);
	} else {
		memcpy(r->buf + pos, src, first);
		memcpy(r->buf, src + first, len - first);
	}

	__atomic_store_n(&r->write_pos, wp + (uint32_t)len, __ATOMIC_RELEASE);
	return OVE_OK;
}

static size_t local_pull_audio(struct ove_sim_transport *t,
				void *samples, size_t len)
{
	struct shm_local_priv *p = (struct shm_local_priv *)t->priv;
	if (!p->audio_in && !p->audio_init_done)
		local_audio_init(p, 16000, 1, 16);
	if (!p->audio_in)
		return 0;

	struct ove_sim_audio_ring *r = p->audio_in;
	uint32_t wpos = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE);
	uint32_t rpos = r->read_pos;
	uint32_t avail = wpos - rpos;
	if (avail > r->size)
		avail = 0;
	if (avail == 0)
		return 0;

	uint32_t to_read = avail < (uint32_t)len ? avail : (uint32_t)len;
	uint32_t mask = ove_sim_ring_mask(r);
	uint32_t pos = rpos & mask;
	uint32_t first = r->size - pos;
	uint8_t *dst = (uint8_t *)samples;

	if (first >= to_read) {
		memcpy(dst, r->buf + pos, to_read);
	} else {
		memcpy(dst, r->buf + pos, first);
		memcpy(dst + first, r->buf, to_read - first);
	}

	__atomic_store_n(&r->read_pos, rpos + to_read, __ATOMIC_RELEASE);
	return to_read;
}

/* ── Vtable ──────────────────────────────────────────────────────── */

static const struct ove_sim_transport_ops shm_local_ops = {
	.open          = local_open,
	.close         = local_close,
	.send_event    = local_send_event,
	.recv_cmd      = local_recv_cmd,
	.flush_display = local_flush_display,
	.push_audio    = local_push_audio,
	.pull_audio    = local_pull_audio,
};

/* ── Public factory ──────────────────────────────────────────────── */

static struct shm_local_priv local_priv_instance;

int ove_sim_transport_shm_local_create(struct ove_sim_transport *t)
{
	if (!t)
		return OVE_ERR_INVALID_PARAM;

	memset(&local_priv_instance, 0, sizeof(local_priv_instance));
	local_priv_instance.sim_fd = -1;
	local_priv_instance.fb_fd = -1;
	local_priv_instance.audio_fd = -1;
	local_priv_instance.fb_map_size = FB_MAX_SIZE;

	t->ops = &shm_local_ops;
	t->priv = &local_priv_instance;
	return OVE_OK;
}

#endif /* !__EMSCRIPTEN__ && !CONFIG_OVE_BOARD_QEMU_MPS2_AN500 */
