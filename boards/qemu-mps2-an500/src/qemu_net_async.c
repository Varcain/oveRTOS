/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 *
 * QEMU MPS2-AN500 shared-memory Ethernet transport for the embassy-net
 * driver. Mirrors the ring protocol used by qemu_net.c (the lwIP
 * backend) but exposes a polling, IP-stack-agnostic API.
 */

#include "ove_config.h"

#ifdef CONFIG_OVE_ASYNC_NET

#include "qemu_net_async.h"
#include "qemu_net_shm.h"
#include "semihosting.h"
#include "ove/types.h"

#include <string.h>
#include <stddef.h>

static int g_fd_tx = -1;
static int g_fd_rx = -1;
static uint32_t g_tx_wpos;
static uint32_t g_rx_rpos;

/* ── Ring helpers (parallel to qemu_net.c's; kept inline here to
 *    avoid pulling in lwIP headers from the embassy-net path) ──── */

static void ring_write_tx(const void *buf, uint32_t len)
{
	uint32_t off = NET_SHM_TX_RING_OFF;
	uint32_t mask = NET_SHM_RING_SIZE - 1;
	uint32_t pos = g_tx_wpos & mask;
	uint32_t first = NET_SHM_RING_SIZE - pos;

	if (first >= len) {
		sh_seek(g_fd_tx, off + pos);
		sh_write(g_fd_tx, buf, len);
	} else {
		sh_seek(g_fd_tx, off + pos);
		sh_write(g_fd_tx, buf, first);
		sh_seek(g_fd_tx, off);
		sh_write(g_fd_tx, (const uint8_t *)buf + first, len - first);
	}
	g_tx_wpos += len;
}

static uint32_t ring_avail_rx(void)
{
	uint32_t rx_wpos;

	sh_seek(g_fd_rx, offsetof(struct net_shm_header, rx_write_pos));
	sh_read(g_fd_rx, &rx_wpos, sizeof(rx_wpos));

	uint32_t avail = rx_wpos - g_rx_rpos;
	if (avail > NET_SHM_RING_SIZE)
		avail = 0;
	return avail;
}

static void ring_read_rx(void *buf, uint32_t len)
{
	uint32_t off = NET_SHM_RX_RING_OFF;
	uint32_t mask = NET_SHM_RING_SIZE - 1;
	uint32_t pos = g_rx_rpos & mask;
	uint32_t first = NET_SHM_RING_SIZE - pos;

	if (first >= len) {
		sh_seek(g_fd_rx, off + pos);
		sh_read(g_fd_rx, buf, len);
	} else {
		sh_seek(g_fd_rx, off + pos);
		sh_read(g_fd_rx, buf, first);
		sh_seek(g_fd_rx, off);
		sh_read(g_fd_rx, (uint8_t *)buf + first, len - first);
	}
	g_rx_rpos += len;
}

static void flush_tx_wpos(void)
{
	sh_seek(g_fd_tx, offsetof(struct net_shm_header, tx_write_pos));
	sh_write(g_fd_tx, &g_tx_wpos, sizeof(g_tx_wpos));
}

static void flush_rx_rpos(void)
{
	sh_seek(g_fd_rx, offsetof(struct net_shm_header, rx_read_pos));
	sh_write(g_fd_rx, &g_rx_rpos, sizeof(g_rx_rpos));
}

/* ── Public API ─────────────────────────────────────────────────── */

int ove_qemu_net_async_init(const uint8_t mac[6])
{
	g_fd_tx = sh_open(NET_SHM_PATH, 3); /* "r+b" */
	g_fd_rx = sh_open(NET_SHM_PATH, 3);
	if (g_fd_tx < 0 || g_fd_rx < 0) {
		g_fd_tx = -1;
		g_fd_rx = -1;
		return OVE_ERR_NOT_FOUND;
	}

	struct net_shm_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = NET_SHM_MAGIC;
	memcpy(hdr.mac_addr, mac, 6);
	hdr.mtu = NET_SHM_MTU;
	hdr.ring_size = NET_SHM_RING_SIZE;

	sh_seek(g_fd_tx, 0);
	sh_write(g_fd_tx, &hdr, sizeof(hdr));

	g_tx_wpos = 0;
	g_rx_rpos = 0;
	return OVE_OK;
}

int ove_qemu_net_async_tx(const void *frame, uint32_t len)
{
	if (g_fd_tx < 0)
		return OVE_ERR_BUS_ERROR;
	if (len == 0 || len > NET_SHM_MTU)
		return OVE_ERR_INVALID_PARAM;

	uint32_t needed = 2 + len;
	uint32_t tx_rpos;
	sh_seek(g_fd_tx, offsetof(struct net_shm_header, tx_read_pos));
	sh_read(g_fd_tx, &tx_rpos, sizeof(tx_rpos));

	uint32_t used = g_tx_wpos - tx_rpos;
	if (used + needed > NET_SHM_RING_SIZE)
		return OVE_ERR_NO_MEMORY;

	uint16_t prefix = (uint16_t)len;
	ring_write_tx(&prefix, sizeof(prefix));
	ring_write_tx(frame, len);
	flush_tx_wpos();
	return OVE_OK;
}

int ove_qemu_net_async_rx(void *buf, uint32_t buf_size, uint32_t *out_len)
{
	if (g_fd_rx < 0)
		return OVE_ERR_BUS_ERROR;

	uint32_t avail = ring_avail_rx();
	if (avail < 2)
		return OVE_ERR_NOT_FOUND;

	uint16_t frame_len;
	ring_read_rx(&frame_len, sizeof(frame_len));

	if (frame_len == 0 || frame_len > NET_SHM_MTU) {
		flush_rx_rpos();
		return OVE_ERR_BUS_ERROR;
	}
	if (avail < 2u + frame_len) {
		g_rx_rpos -= 2;
		return OVE_ERR_NOT_FOUND;
	}
	if (frame_len > buf_size) {
		/* Drop oversize frame to keep the ring moving. */
		g_rx_rpos += frame_len;
		flush_rx_rpos();
		return OVE_ERR_NO_MEMORY;
	}

	ring_read_rx(buf, frame_len);
	flush_rx_rpos();
	*out_len = frame_len;
	return OVE_OK;
}

int ove_qemu_net_async_link_up(void)
{
	if (g_fd_rx < 0)
		return 0;
	uint8_t up = 0;
	sh_seek(g_fd_rx, offsetof(struct net_shm_header, link_up));
	sh_read(g_fd_rx, &up, sizeof(up));
	return up;
}

#endif /* CONFIG_OVE_ASYNC_NET */
