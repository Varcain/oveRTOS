/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Engine-neutral ownership policy for a removable storage medium.
 */

#ifndef OVE_MEDIA_H
#define OVE_MEDIA_H

#include "ove/types.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVE_MEDIA_RAW_WRITE 0x01u

/** A generation-qualified raw-media lease. Do not copy an active lease. */
struct ove_media_lease {
	uint32_t generation;
	uint8_t access;
	uint8_t active;
	uint8_t _reserved[2];
};

/** Publish the currently observed card generation and presence state. */
void ove_media_observe(uint32_t generation, int present);
/** Mark the last observed generation absent without inventing a new identity. */
void ove_media_removed(void);

/** Acquire/release the single native-filesystem ownership slot. */
int ove_media_fs_acquire(void);
void ove_media_fs_release(void);

/** Acquire/release a raw reader or an exclusive raw writer. */
int ove_media_raw_acquire(struct ove_media_lease *lease, unsigned flags, uint32_t generation);
void ove_media_raw_release(struct ove_media_lease *lease);

/** True only while the lease still refers to the present card generation. */
int ove_media_raw_valid(const struct ove_media_lease *lease, unsigned required_flags);

#ifdef __cplusplus
}
#endif

#endif /* OVE_MEDIA_H */
