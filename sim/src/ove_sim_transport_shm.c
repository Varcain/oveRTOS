/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Shared-memory transport for QEMU simulation mode.
 *
 * Uses a mmap'd file (/dev/shm/ove-sim) with the layout defined in
 * ove_sim_shm.h.  The QEMU guest firmware writes events and reads
 * commands; this host-side transport reads events and writes commands.
 *
 * Both directions use SPSC ring buffers with positioned I/O.
 * No mutexes — producer/consumer synchronisation relies on ordered
 * position updates (write data first, then update write_pos).
 */

#include "ove/sim/ove_sim_transport.h"
#include "ove/sim/ove_sim_shm.h"
#include "ove/types.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Private state ────────────────────────────────────────────────── */

struct shm_priv {
	int      fd;
	uint8_t *base;          /* mmap'd region */
	size_t   map_size;
	uint32_t event_rpos;    /* host-side read position for events */
	uint32_t cmd_wpos;      /* host-side write position for commands */
};

/* ── Ring helpers (no locks — SPSC with ordered stores) ───────────── */

static uint32_t shm_ring_available(const uint8_t *base, uint32_t wpos_off,
				   uint32_t rpos)
{
	uint32_t wpos;
	memcpy(&wpos, base + wpos_off, sizeof(wpos));
	return wpos - rpos;
}

/* ── Transport ops ────────────────────────────────────────────────── */

static int shm_transport_open(struct ove_sim_transport *t, const char *endpoint)
{
	(void)endpoint;
	struct shm_priv *p = (struct shm_priv *)t->priv;

	/* Wait for the guest to create and initialise the SHM file. */
	int fd = -1;
	for (int attempt = 0; attempt < 300; attempt++) { /* 30 s max */
		fd = open(SIM_SHM_PATH, O_RDWR);
		if (fd >= 0)
			break;
		usleep(100000); /* 100 ms */
	}
	if (fd < 0) {
		fprintf(stderr, "[sim-shm] Cannot open %s\n", SIM_SHM_PATH);
		return OVE_ERR_NOT_SUPPORTED;
	}

	/* Ensure the file is large enough. */
	struct stat st;
	fstat(fd, &st);
	if ((size_t)st.st_size < SIM_SHM_TOTAL_SIZE) {
		if (ftruncate(fd, SIM_SHM_TOTAL_SIZE) < 0) {
			close(fd);
			return OVE_ERR_NOT_SUPPORTED;
		}
	}

	uint8_t *base = mmap(NULL, SIM_SHM_TOTAL_SIZE,
			     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (base == MAP_FAILED) {
		close(fd);
		return OVE_ERR_NO_MEMORY;
	}

	/* Wait for magic to appear (guest writes it during init). */
	for (int attempt = 0; attempt < 300; attempt++) {
		uint32_t magic;
		memcpy(&magic, base, sizeof(magic));
		if (magic == SIM_SHM_MAGIC)
			goto ready;
		usleep(100000);
	}
	munmap(base, SIM_SHM_TOTAL_SIZE);
	close(fd);
	return OVE_ERR_TIMEOUT;

ready:
	p->fd = fd;
	p->base = base;
	p->map_size = SIM_SHM_TOTAL_SIZE;

	/* Start reading from the current guest write positions. */
	memcpy(&p->event_rpos, base + offsetof(struct sim_shm_header,
					       event_write_pos), 4);
	memcpy(&p->cmd_wpos, base + offsetof(struct sim_shm_header,
					      cmd_write_pos), 4);

	return OVE_OK;
}

static void shm_transport_close(struct ove_sim_transport *t)
{
	struct shm_priv *p = (struct shm_priv *)t->priv;
	if (p->base) {
		munmap(p->base, p->map_size);
		p->base = NULL;
	}
	if (p->fd >= 0) {
		close(p->fd);
		p->fd = -1;
	}
}

/*
 * Firmware-side send_event / recv_cmd are not used on the host side.
 * They exist for completeness but return errors — the host uses
 * read_event / write_cmd instead.
 */
static int shm_send_event(struct ove_sim_transport *t,
			   const struct ove_sim_event *event)
{
	(void)t; (void)event;
	return OVE_ERR_NOT_SUPPORTED;
}

static int shm_recv_cmd(struct ove_sim_transport *t,
			struct ove_sim_cmd *cmd, size_t cmd_size,
			uint32_t timeout_ms)
{
	(void)t; (void)cmd; (void)cmd_size; (void)timeout_ms;
	return OVE_ERR_NOT_SUPPORTED;
}

/* ── Host-side ops ────────────────────────────────────────────────── */

/**
 * Read the next event from the event ring (guest → host).
 * Messages are stored as [uint16_t len][data[len]].
 */
static int shm_read_event(struct ove_sim_transport *t,
			   void *buf, size_t buf_size,
			   uint16_t *out_len, uint32_t timeout_ms)
{
	struct shm_priv *p = (struct shm_priv *)t->priv;
	uint8_t *ring = p->base + SIM_SHM_EVENT_RING_OFF;
	uint32_t mask = SIM_SHM_RING_SIZE - 1;

	/* Poll for data (timeout_ms == 0 means non-blocking). */
	uint32_t avail;
	for (;;) {
		uint32_t wpos;
		memcpy(&wpos, p->base + offsetof(struct sim_shm_header,
						  event_write_pos), 4);
		avail = wpos - p->event_rpos;
		if (avail >= sizeof(uint16_t))
			break;
		if (timeout_ms == 0)
			return OVE_ERR_TIMEOUT;
		usleep(1000); /* 1 ms */
		if (timeout_ms != UINT32_MAX) {
			if (timeout_ms <= 1)
				return OVE_ERR_TIMEOUT;
			timeout_ms--;
		}
	}

	/* Read length prefix. */
	uint32_t rp = p->event_rpos;
	uint16_t len = ring[rp & mask];
	rp++;
	len |= (uint16_t)ring[rp & mask] << 8;
	rp++;

	if (len > buf_size) {
		/* Skip oversized message. */
		p->event_rpos = rp + len;
		memcpy(p->base + offsetof(struct sim_shm_header,
					  event_read_pos),
		       &p->event_rpos, 4);
		*out_len = 0;
		return OVE_ERR_INVALID_PARAM;
	}

	/* Read payload. */
	uint8_t *dst = (uint8_t *)buf;
	for (uint16_t i = 0; i < len; i++) {
		dst[i] = ring[(rp + i) & mask];
	}
	rp += len;

	p->event_rpos = rp;
	/* Update read position so the guest can see consumption. */
	memcpy(p->base + offsetof(struct sim_shm_header, event_read_pos),
	       &p->event_rpos, 4);

	*out_len = len;
	return OVE_OK;
}

/**
 * Write a command into the command ring (host → guest).
 */
static int shm_write_cmd(struct ove_sim_transport *t,
			  const void *data, uint16_t len)
{
	struct shm_priv *p = (struct shm_priv *)t->priv;
	uint8_t *ring = p->base + SIM_SHM_CMD_RING_OFF;
	uint32_t mask = SIM_SHM_RING_SIZE - 1;
	uint32_t total = (uint32_t)sizeof(uint16_t) + len;

	/* Check free space. */
	uint32_t rpos;
	memcpy(&rpos, p->base + offsetof(struct sim_shm_header,
					  cmd_read_pos), 4);
	uint32_t free = SIM_SHM_RING_SIZE - (p->cmd_wpos - rpos);
	if (free < total)
		return OVE_ERR_QUEUE_FULL;

	/* Write length prefix. */
	uint32_t wp = p->cmd_wpos;
	ring[wp & mask] = (uint8_t)(len & 0xFF);
	wp++;
	ring[wp & mask] = (uint8_t)((len >> 8) & 0xFF);
	wp++;

	/* Write payload. */
	const uint8_t *src = (const uint8_t *)data;
	for (uint16_t i = 0; i < len; i++) {
		ring[wp & mask] = src[i];
		wp++;
	}

	p->cmd_wpos = wp;
	/* Update write position so the guest can see the new command. */
	memcpy(p->base + offsetof(struct sim_shm_header, cmd_write_pos),
	       &p->cmd_wpos, 4);

	return OVE_OK;
}

static const struct ove_sim_transport_ops shm_ops = {
	.open       = shm_transport_open,
	.close      = shm_transport_close,
	.send_event = shm_send_event,
	.recv_cmd   = shm_recv_cmd,
	.read_event = shm_read_event,
	.write_cmd  = shm_write_cmd,
};

/* ── Public factory ───────────────────────────────────────────────── */

static struct shm_priv shm_priv_instance;

int ove_sim_transport_shm_create(struct ove_sim_transport *t)
{
	if (!t)
		return OVE_ERR_INVALID_PARAM;

	memset(&shm_priv_instance, 0, sizeof(shm_priv_instance));
	shm_priv_instance.fd = -1;

	t->ops = &shm_ops;
	t->priv = &shm_priv_instance;
	return OVE_OK;
}
