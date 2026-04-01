/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU shared-memory networking protocol.
 *
 * The guest firmware sends and receives Ethernet frames through a
 * shared-memory file (/dev/shm/ove-net) using ARM semihosting.
 * The host-side bridge (qemu-net-bridge.py) mmaps the same file
 * and forwards frames to/from a TAP interface.
 *
 * Layout:
 *   [0 .. 63]                          net_shm_header
 *   [64 .. 64+RING_SIZE-1]             TX ring (guest -> host)
 *   [64+RING_SIZE .. 64+2*RING_SIZE-1] RX ring (host -> guest)
 *
 * Each ring is an SPSC (single-producer single-consumer) byte buffer.
 * Frames are stored as [uint16_t length][uint8_t data[length]].
 * Positions are byte offsets that wrap modulo ring_size.
 */

#ifndef QEMU_NET_SHM_H
#define QEMU_NET_SHM_H

#include <stdint.h>

#define NET_SHM_MAGIC      0x4F564E54  /* "OVNT" little-endian */
#define NET_SHM_RING_SIZE  (1u << 16)  /* 64 KB per direction */
#define NET_SHM_HDR_SIZE   64
#define NET_SHM_MTU        1518        /* Max Ethernet frame size */

#define NET_SHM_TX_RING_OFF  NET_SHM_HDR_SIZE
#define NET_SHM_RX_RING_OFF  (NET_SHM_HDR_SIZE + NET_SHM_RING_SIZE)
#define NET_SHM_TOTAL_SIZE   (NET_SHM_HDR_SIZE + 2u * NET_SHM_RING_SIZE)

#define NET_SHM_PATH  "/dev/shm/ove-net"

struct net_shm_header {
	uint32_t magic;              /* NET_SHM_MAGIC */
	uint8_t  mac_addr[6];       /* Guest MAC address */
	uint16_t mtu;               /* Max frame size (1518) */
	uint32_t ring_size;         /* Bytes per direction */
	/* TX: guest writes, host reads */
	uint32_t tx_write_pos;
	uint32_t tx_read_pos;
	/* RX: host writes, guest reads */
	uint32_t rx_write_pos;
	uint32_t rx_read_pos;
	uint8_t  link_up;           /* Host sets to 1 when bridge ready */
	uint8_t  _pad[23];          /* Pad to 64 bytes */
};

#endif /* QEMU_NET_SHM_H */
