/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * QEMU shared-memory audio protocol.
 *
 * The guest firmware writes output PCM and reads input PCM through a
 * shared-memory file (/dev/shm/ove-audio) using ARM semihosting.
 * The host-side viewer (qemu-dashboard-bridge.py) mmaps the same file
 * for audio playback and capture.
 *
 * Layout:
 *   [0 .. 63]                        audio_shm_header
 *   [64 .. 64+RING_SIZE-1]           Output ring (guest -> host)
 *   [64+RING_SIZE .. 64+2*RING_SIZE-1] Input ring  (host -> guest)
 *
 * Each ring is an SPSC (single-producer single-consumer) byte buffer.
 * Positions are byte offsets that wrap modulo ring_size.
 */

#ifndef QEMU_AUDIO_SHM_H
#define QEMU_AUDIO_SHM_H

#include <stdint.h>

#define AUDIO_SHM_MAGIC      0x4F564155  /* "OVAU" little-endian */
#define AUDIO_SHM_RING_SIZE  (1u << 17)  /* 128 KB per direction */
#define AUDIO_SHM_HDR_SIZE   64

#define AUDIO_SHM_OUT_RING_OFF  AUDIO_SHM_HDR_SIZE
#define AUDIO_SHM_IN_RING_OFF   (AUDIO_SHM_HDR_SIZE + AUDIO_SHM_RING_SIZE)
#define AUDIO_SHM_TOTAL_SIZE    (AUDIO_SHM_HDR_SIZE + 2u * AUDIO_SHM_RING_SIZE)

#define AUDIO_SHM_PATH  "/dev/shm/ove-audio"

struct audio_shm_header {
	uint32_t magic;              /* AUDIO_SHM_MAGIC */
	uint32_t sample_rate;        /* Hz, e.g. 48000 */
	uint16_t channels;           /* e.g. 2 */
	uint16_t bit_depth;          /* e.g. 16 */
	uint32_t frames_per_buffer;  /* frames per callback */
	uint32_t ring_size;          /* bytes per direction */
	/* Output: guest writes, host reads */
	uint32_t out_write_pos;
	uint32_t out_read_pos;
	/* Input: host writes, guest reads */
	uint32_t in_write_pos;
	uint32_t in_read_pos;
	uint8_t  _pad[16];          /* pad to 64 bytes */
};

#endif /* QEMU_AUDIO_SHM_H */
