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
 * The host-side viewer (ove-dashboard-bridge.py) mmaps the same file
 * for audio playback and capture.
 *
 * Uses the common ove_sim_audio_ring layout (32-byte header + ring buf)
 * so all sim transports share the same struct.
 *
 * Layout:
 *   [0 .. HDR+RING-1]              Output ring (guest -> host)
 *   [HDR+RING .. 2*(HDR+RING)-1]   Input ring  (host -> guest)
 *
 * Each ring is an SPSC (single-producer single-consumer) byte buffer.
 * Positions are byte offsets that wrap modulo ring_size.
 */

#ifndef QEMU_AUDIO_SHM_H
#define QEMU_AUDIO_SHM_H

#include <stdint.h>

/* Use the common ring offset constants. */
#define AUDIO_SHM_MAGIC 0x4F564155     /* "OVAU" little-endian */
#define AUDIO_SHM_RING_SIZE (1u << 16) /* 64 KB per direction */
#define AUDIO_SHM_RING_HDR 32	       /* matches OVE_RING_OFF_BUF */

#define AUDIO_SHM_RING_TOTAL (AUDIO_SHM_RING_HDR + AUDIO_SHM_RING_SIZE)
#define AUDIO_SHM_OUT_RING_OFF 0
#define AUDIO_SHM_IN_RING_OFF AUDIO_SHM_RING_TOTAL
#define AUDIO_SHM_TOTAL_SIZE (2u * AUDIO_SHM_RING_TOTAL)

#define AUDIO_SHM_PATH "/dev/shm/ove-audio"

/*
 * Each ring in the SHM file has the same layout as struct ove_sim_audio_ring:
 *
 *   Offset  Size  Field
 *   0       4     write_pos
 *   4       4     read_pos
 *   8       4     sample_rate
 *   12      2     channels
 *   14      2     bit_depth
 *   16      4     size (= AUDIO_SHM_RING_SIZE)
 *   20      4     underruns
 *   24      4     overruns
 *   28      4     _reserved
 *   32      N     buf[N]
 *
 * Guest accesses these via semihosting using the OVE_RING_OFF_* constants
 * from ove_sim_audio_ring.h.  Host accesses via mmap.
 */

/* Field offsets within each ring (mirrors OVE_RING_OFF_*) */
#define QEMU_RING_OFF_WRITE_POS 0
#define QEMU_RING_OFF_READ_POS 4
#define QEMU_RING_OFF_SAMPLE_RATE 8
#define QEMU_RING_OFF_CHANNELS 12
#define QEMU_RING_OFF_BIT_DEPTH 14
#define QEMU_RING_OFF_SIZE 16
#define QEMU_RING_OFF_BUF 32

#endif /* QEMU_AUDIO_SHM_H */
