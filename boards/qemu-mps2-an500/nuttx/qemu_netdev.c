/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * eth0 for NuttX on QEMU MPS2-AN500: a netdev lower half over the same
 * shared-memory frame rings the FreeRTOS lwIP driver uses (qemu_net_shm.h).
 * QEMU's own LAN9118 has no NuttX driver; the rings live in /dev/shm/ove-net,
 * reached through semihosting, and qemu-net-bridge.py connects them to a TAP
 * interface on the host.
 *
 * Without the bridge the device still registers, comes up with no carrier and
 * drops what it is given, so network configuration succeeds either way. RX is
 * polled: a watchdog timer queues low-priority work every few milliseconds,
 * which checks the ring and wakes the upper half. All semihosting calls run
 * on work threads, never in interrupt context.
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/clock.h>
#include <nuttx/net/netdev_lowerhalf.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "qemu_net_shm.h"
#include "semihosting.h"

#define QNET_POLL_MS 2
#define QNET_QUOTA 4

struct qnet {
	struct netdev_lowerhalf_s dev;
	struct wdog_s poll_wd;
	struct work_s poll_work;
	int fd_tx;	  /* TX ring operations (upper half's TX path) */
	int fd_rx;	  /* RX ring and header reads (RX path, poll work) */
	uint32_t tx_wpos; /* mirrors header.tx_write_pos */
	uint32_t rx_rpos; /* mirrors header.rx_read_pos */
	bool up;
	bool carrier;
	uint8_t tx_buf[NET_SHM_MTU];
	uint8_t rx_buf[NET_SHM_MTU];
};

static struct qnet g_qnet = {.fd_tx = -1, .fd_rx = -1};

static void ring_io(int fd, uint32_t base, uint32_t *pos, void *buf, uint32_t len, bool write)
{
	uint32_t at = *pos & (NET_SHM_RING_SIZE - 1u);
	uint32_t first = NET_SHM_RING_SIZE - at;
	uint8_t *p = buf;

	if (first > len)
		first = len;
	sh_seek(fd, base + at);
	if (write)
		sh_write(fd, p, first);
	else
		sh_read(fd, p, first);
	if (len > first) {
		sh_seek(fd, base);
		if (write)
			sh_write(fd, p + first, len - first);
		else
			sh_read(fd, p + first, len - first);
	}
	*pos += len;
}

static uint32_t header_u32(int fd, size_t offset)
{
	uint32_t v = 0;
	sh_seek(fd, (uint32_t)offset);
	sh_read(fd, &v, sizeof(v));
	return v;
}

static void header_put_u32(int fd, size_t offset, uint32_t v)
{
	sh_seek(fd, (uint32_t)offset);
	sh_write(fd, &v, sizeof(v));
}

static uint32_t rx_avail(struct qnet *q)
{
	uint32_t avail =
		header_u32(q->fd_rx, offsetof(struct net_shm_header, rx_write_pos)) - q->rx_rpos;
	return avail > NET_SHM_RING_SIZE ? 0 : avail; /* lapped: treat as empty */
}

/* ── RX polling ──────────────────────────────────────────────────────── */

static void poll_expiry(wdparm_t arg);

static void poll_work(FAR void *arg)
{
	struct qnet *q = arg;
	if (!q->up)
		return;

	/* The bridge raises link_up once it has attached the TAP. */
	uint8_t link_up = 0;
	sh_seek(q->fd_rx, offsetof(struct net_shm_header, link_up));
	sh_read(q->fd_rx, &link_up, sizeof(link_up));
	if (link_up && !q->carrier) {
		q->carrier = true;
		netdev_lower_carrier_on(&q->dev);
	}

	if (rx_avail(q) >= 2)
		netdev_lower_rxready(&q->dev);
	wd_start(&q->poll_wd, MSEC2TICK(QNET_POLL_MS), poll_expiry, (wdparm_t)q);
}

static void poll_expiry(wdparm_t arg)
{
	struct qnet *q = (struct qnet *)arg;
	work_queue(LPWORK, &q->poll_work, poll_work, q, 0);
}

/* ── Lower-half operations ──────────────────────────────────────────── */

static int qnet_ifup(FAR struct netdev_lowerhalf_s *dev)
{
	struct qnet *q = (struct qnet *)dev;
	if (q->fd_tx < 0)
		return OK; /* no rings (no bridge file): up, without a carrier */

	struct net_shm_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = NET_SHM_MAGIC;
	memcpy(hdr.mac_addr, dev->netdev.d_mac.ether.ether_addr_octet, sizeof(hdr.mac_addr));
	hdr.mtu = NET_SHM_MTU;
	hdr.ring_size = NET_SHM_RING_SIZE;
	sh_seek(q->fd_tx, 0);
	sh_write(q->fd_tx, &hdr, sizeof(hdr));
	q->tx_wpos = 0;
	q->rx_rpos = 0;

	q->up = true;
	wd_start(&q->poll_wd, MSEC2TICK(QNET_POLL_MS), poll_expiry, (wdparm_t)q);
	return OK;
}

static int qnet_ifdown(FAR struct netdev_lowerhalf_s *dev)
{
	struct qnet *q = (struct qnet *)dev;
	q->up = false;
	wd_cancel(&q->poll_wd);
	work_cancel(LPWORK, &q->poll_work);
	if (q->carrier) {
		q->carrier = false;
		netdev_lower_carrier_off(dev);
	}
	return OK;
}

static int qnet_transmit(FAR struct netdev_lowerhalf_s *dev, FAR netpkt_t *pkt)
{
	struct qnet *q = (struct qnet *)dev;
	unsigned int len = netpkt_getdatalen(dev, pkt);

	if (q->fd_tx >= 0 && q->carrier && len > 0 && len <= NET_SHM_MTU) {
		uint32_t used = q->tx_wpos -
				header_u32(q->fd_tx, offsetof(struct net_shm_header, tx_read_pos));
		/* A full ring drops the frame, as a busy wire would. */
		if (used + 2u + len <= NET_SHM_RING_SIZE) {
			uint16_t len16 = (uint16_t)len;
			netpkt_copyout(dev, q->tx_buf, pkt, len, 0);
			ring_io(q->fd_tx, NET_SHM_TX_RING_OFF, &q->tx_wpos, &len16, sizeof(len16),
				true);
			ring_io(q->fd_tx, NET_SHM_TX_RING_OFF, &q->tx_wpos, q->tx_buf, len, true);
			header_put_u32(q->fd_tx, offsetof(struct net_shm_header, tx_write_pos),
				       q->tx_wpos);
		}
	}
	netpkt_free(dev, pkt, NETPKT_TX);
	netdev_lower_txdone(dev);
	return OK;
}

static FAR netpkt_t *qnet_receive(FAR struct netdev_lowerhalf_s *dev)
{
	struct qnet *q = (struct qnet *)dev;
	if (q->fd_rx < 0)
		return NULL;

	uint32_t avail = rx_avail(q);
	if (avail < 2)
		return NULL;

	uint16_t len;
	uint32_t at = q->rx_rpos;
	ring_io(q->fd_rx, NET_SHM_RX_RING_OFF, &at, &len, sizeof(len), false);
	if (len == 0 || len > NET_SHM_MTU) {
		/* Protocol error: drop everything queued and resynchronise. */
		q->rx_rpos += avail;
		header_put_u32(q->fd_rx, offsetof(struct net_shm_header, rx_read_pos), q->rx_rpos);
		return NULL;
	}
	if (avail < 2u + len)
		return NULL; /* the rest of the frame is not there yet */

	ring_io(q->fd_rx, NET_SHM_RX_RING_OFF, &at, q->rx_buf, len, false);
	q->rx_rpos = at;
	header_put_u32(q->fd_rx, offsetof(struct net_shm_header, rx_read_pos), q->rx_rpos);

	FAR netpkt_t *pkt = netpkt_alloc(dev, NETPKT_RX);
	if (pkt == NULL)
		return NULL; /* out of buffers: the frame is lost */
	if (netpkt_copyin(dev, pkt, q->rx_buf, len, 0) < 0) {
		netpkt_free(dev, pkt, NETPKT_RX);
		return NULL;
	}
	return pkt;
}

static const struct netdev_ops_s g_qnet_ops = {
	.ifup = qnet_ifup,
	.ifdown = qnet_ifdown,
	.transmit = qnet_transmit,
	.receive = qnet_receive,
};

int qemu_netdev_init(void)
{
	struct qnet *q = &g_qnet;

	/* Two descriptors so the TX and RX paths never move each other's file
	 * position. Mode 3 is "r+b": read/write without truncating. */
	q->fd_tx = sh_open(NET_SHM_PATH, 3);
	q->fd_rx = sh_open(NET_SHM_PATH, 3);
	if (q->fd_tx < 0 || q->fd_rx < 0) {
		q->fd_tx = -1;
		q->fd_rx = -1;
	}

	/* Locally administered MAC; the FreeRTOS driver uses ...:01. */
	static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0xBE, 0xEF, 0x02};
	memcpy(q->dev.netdev.d_mac.ether.ether_addr_octet, mac, sizeof(mac));
	q->dev.ops = &g_qnet_ops;
	q->dev.quota[NETPKT_TX] = QNET_QUOTA;
	q->dev.quota[NETPKT_RX] = QNET_QUOTA;
	return netdev_lower_register(&q->dev, NET_LL_ETHERNET);
}
