/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/**
 * @defgroup ove_sim_shm Simulation Shared Memory Layout
 * @brief Shared-memory IPC layout for QEMU simulation transport.
 *
 * Follows the SPSC ring pattern established by @c qemu_audio_shm.h
 * and @c qemu_net_shm.h.  The guest firmware writes events and reads
 * commands; the host daemon reads events and writes commands.
 *
 * Layout:
 *   [0 .. 63]                             sim_shm_header
 *   [64 .. 64+RING_SIZE-1]                Event ring (guest -> host)
 *   [64+RING_SIZE .. 64+2*RING_SIZE-1]    Command ring (host -> guest)
 * @{
 */

#ifndef OVE_SIM_SHM_H
#define OVE_SIM_SHM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_SHM_MAGIC      0x4F565349  /**< "OVSI" little-endian. */
#define SIM_SHM_VERSION    1
#define SIM_SHM_RING_SIZE  (1u << 16)  /**< 64 KB per direction. */
#define SIM_SHM_HDR_SIZE   64

#define SIM_SHM_EVENT_RING_OFF  SIM_SHM_HDR_SIZE
#define SIM_SHM_CMD_RING_OFF    (SIM_SHM_HDR_SIZE + SIM_SHM_RING_SIZE)
#define SIM_SHM_TOTAL_SIZE      (SIM_SHM_HDR_SIZE + 2u * SIM_SHM_RING_SIZE)

#define SIM_SHM_PATH  "/dev/shm/ove-sim"

/**
 * @brief Shared-memory header for the simulation transport.
 *
 * All positions are byte offsets that wrap modulo @c ring_size.
 * Messages are stored as [uint16_t length][uint8_t data[length]].
 */
struct sim_shm_header {
	uint32_t magic;           /**< Must be @c SIM_SHM_MAGIC. */
	uint32_t version;         /**< Protocol version. */
	uint32_t ring_size;       /**< Bytes per ring (both directions). */
	uint32_t plugin_count;    /**< Number of registered plugins. */

	/* Event ring: guest writes, host reads. */
	uint32_t event_write_pos; /**< Next write position (guest). */
	uint32_t event_read_pos;  /**< Next read position (host). */

	/* Command ring: host writes, guest reads. */
	uint32_t cmd_write_pos;   /**< Next write position (host). */
	uint32_t cmd_read_pos;    /**< Next read position (guest). */

	uint8_t  _pad[28];        /**< Pad to 64 bytes. */
};

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* OVE_SIM_SHM_H */
