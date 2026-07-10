/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU MPS2-AN500 shared-memory Ethernet driver for lwIP.
 *
 * Bridges Ethernet frames between the lwIP stack and a host-side
 * TAP interface through /dev/shm/ove-net via ARM semihosting.
 *
 * Two semihosting file descriptors are used (one for TX, one for RX)
 * to avoid seek-position conflicts between the output path (called
 * from any lwIP task) and the input path (called from eth_rx_task).
 *
 * When the shared-memory file cannot be opened (headless / no bridge),
 * the driver falls back to a silent no-op — identical to the previous
 * stub behavior.
 */

#include "lwip/opt.h"

#if LWIP_SOCKET /* Only compile when lwIP is enabled */

#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "semihosting.h"
#include "qemu_net_shm.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>
#include <stddef.h>

/* ── Semihosting file descriptors (separate to avoid seek conflicts) ── */

static int g_sh_fd_tx = -1; /* TX ring operations */
static int g_sh_fd_rx = -1; /* RX ring operations */

/* ── Local ring position tracking ────────────────────────────────────── */

static uint32_t g_tx_wpos; /* Mirrors header.tx_write_pos */
static uint32_t g_rx_rpos; /* Mirrors header.rx_read_pos */

/* ── Scratch buffers (separate for TX/RX to avoid preemption races) ── */

static uint8_t g_tx_buf[NET_SHM_MTU];
static uint8_t g_rx_buf[NET_SHM_MTU];

/* ── Ring helpers (modeled on qemu_audio.c ring_write_out/ring_read_in) */

static void ring_write_tx(const void *buf, uint32_t len)
{
	uint32_t off = NET_SHM_TX_RING_OFF;
	uint32_t mask = NET_SHM_RING_SIZE - 1;
	uint32_t pos = g_tx_wpos & mask;
	uint32_t first = NET_SHM_RING_SIZE - pos;

	if (first >= len) {
		sh_seek(g_sh_fd_tx, off + pos);
		sh_write(g_sh_fd_tx, buf, len);
	} else {
		sh_seek(g_sh_fd_tx, off + pos);
		sh_write(g_sh_fd_tx, buf, first);
		sh_seek(g_sh_fd_tx, off);
		sh_write(g_sh_fd_tx, (const uint8_t *)buf + first, len - first);
	}
	g_tx_wpos += len;
}

static uint32_t ring_avail_rx(void)
{
	uint32_t rx_wpos;

	sh_seek(g_sh_fd_rx, offsetof(struct net_shm_header, rx_write_pos));
	sh_read(g_sh_fd_rx, &rx_wpos, sizeof(rx_wpos));

	uint32_t avail = rx_wpos - g_rx_rpos;
	if (avail > NET_SHM_RING_SIZE)
		avail = 0; /* wrapped past — treat as empty */
	return avail;
}

static void ring_read_rx(void *buf, uint32_t len)
{
	uint32_t off = NET_SHM_RX_RING_OFF;
	uint32_t mask = NET_SHM_RING_SIZE - 1;
	uint32_t pos = g_rx_rpos & mask;
	uint32_t first = NET_SHM_RING_SIZE - pos;

	if (first >= len) {
		sh_seek(g_sh_fd_rx, off + pos);
		sh_read(g_sh_fd_rx, buf, len);
	} else {
		sh_seek(g_sh_fd_rx, off + pos);
		sh_read(g_sh_fd_rx, buf, first);
		sh_seek(g_sh_fd_rx, off);
		sh_read(g_sh_fd_rx, (uint8_t *)buf + first, len - first);
	}
	g_rx_rpos += len;
}

static void flush_tx_wpos(void)
{
	sh_seek(g_sh_fd_tx, offsetof(struct net_shm_header, tx_write_pos));
	sh_write(g_sh_fd_tx, &g_tx_wpos, sizeof(g_tx_wpos));
}

static void flush_rx_rpos(void)
{
	sh_seek(g_sh_fd_rx, offsetof(struct net_shm_header, rx_read_pos));
	sh_write(g_sh_fd_rx, &g_rx_rpos, sizeof(g_rx_rpos));
}

/* ── lwIP netif callbacks ────────────────────────────────────────────── */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
	(void)netif;

	if (g_sh_fd_tx < 0)
		return ERR_IF;

	/* Check ring space: 2-byte length prefix + frame */
	uint32_t needed = 2 + p->tot_len;
	uint32_t tx_rpos;

	sh_seek(g_sh_fd_tx, offsetof(struct net_shm_header, tx_read_pos));
	sh_read(g_sh_fd_tx, &tx_rpos, sizeof(tx_rpos));

	uint32_t used = g_tx_wpos - tx_rpos;
	if (used + needed > NET_SHM_RING_SIZE)
		return ERR_MEM; /* Ring full — drop frame */

	/* Linearize pbuf chain */
	uint16_t len = (uint16_t)p->tot_len;
	pbuf_copy_partial(p, g_tx_buf, len, 0);

	/* Write [length][data] to TX ring */
	ring_write_tx(&len, sizeof(len));
	ring_write_tx(g_tx_buf, len);
	flush_tx_wpos();

	return ERR_OK;
}

/* ── Public: lwIP netif init (overrides weak symbol in freertos_net.c) */

err_t ethernetif_init(struct netif *netif)
{
#if LWIP_NETIF_HOSTNAME
	netif->hostname = "overtos-qemu";
#endif
	netif->name[0] = 'q';
	netif->name[1] = 'e';
	netif->output = etharp_output;
	netif->linkoutput = low_level_output;

	/* Locally-administered MAC */
	netif->hwaddr_len = 6;
	netif->hwaddr[0] = 0x02;
	netif->hwaddr[1] = 0x00;
	netif->hwaddr[2] = 0x00;
	netif->hwaddr[3] = 0xBE;
	netif->hwaddr[4] = 0xEF;
	netif->hwaddr[5] = 0x01;

	netif->mtu = 1500;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

	/* Open shared-memory file (two FDs to avoid seek conflicts).
	 * Mode 3 = "r+b" (read/write binary, no truncation).
	 * Mode 7 = "w+b" would truncate — must not use. */
	g_sh_fd_tx = sh_open(NET_SHM_PATH, 3); /* "r+b" */
	g_sh_fd_rx = sh_open(NET_SHM_PATH, 3);

	if (g_sh_fd_tx < 0 || g_sh_fd_rx < 0) {
		/* Headless / no bridge — fall back to silent mode */
		g_sh_fd_tx = -1;
		g_sh_fd_rx = -1;
		return ERR_OK;
	}

	/* Write SHM header so the host bridge can discover us */
	struct net_shm_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = NET_SHM_MAGIC;
	memcpy(hdr.mac_addr, netif->hwaddr, 6);
	hdr.mtu = NET_SHM_MTU;
	hdr.ring_size = NET_SHM_RING_SIZE;

	sh_seek(g_sh_fd_tx, 0);
	sh_write(g_sh_fd_tx, &hdr, sizeof(hdr));

	g_tx_wpos = 0;
	g_rx_rpos = 0;

	/* Wait for the host bridge to set link_up (up to 5 seconds) */
	uint8_t link_up = 0;
	for (int i = 0; i < 50 && !link_up; i++) {
		vTaskDelay(pdMS_TO_TICKS(100));
		sh_seek(g_sh_fd_rx, offsetof(struct net_shm_header, link_up));
		sh_read(g_sh_fd_rx, &link_up, sizeof(link_up));
	}

	if (link_up) {
		netif->flags |= NETIF_FLAG_LINK_UP;
	}

	return ERR_OK;
}

/* ── Public: poll for received frames (overrides weak in freertos_net.c) */

int ethernetif_input(struct netif *netif)
{
	int n = 0;

	if (g_sh_fd_rx < 0)
		return 0;

	/* Process all available frames in this poll cycle */
	for (;;) {
		uint32_t avail = ring_avail_rx();
		if (avail < 2)
			break; /* Not even a length prefix available */

		/* Peek at the frame length */
		uint16_t frame_len;
		ring_read_rx(&frame_len, sizeof(frame_len));

		if (frame_len == 0 || frame_len > NET_SHM_MTU) {
			/* Protocol error — resync by skipping */
			flush_rx_rpos();
			break;
		}

		if (avail < 2 + frame_len) {
			/* Incomplete frame — rewind the 2 bytes we read */
			g_rx_rpos -= 2;
			break;
		}

		/* Read frame data */
		ring_read_rx(g_rx_buf, frame_len);

		/* Allocate pbuf and pass to lwIP */
		struct pbuf *p = pbuf_alloc(PBUF_RAW, frame_len, PBUF_POOL);
		if (p) {
			pbuf_take(p, g_rx_buf, frame_len);
			if (netif->input(p, netif) != ERR_OK)
				pbuf_free(p);
			n++;
		}
		/* If pbuf allocation fails, frame is silently dropped */
	}

	flush_rx_rpos();
	return n;
}

#endif /* LWIP_SOCKET */
