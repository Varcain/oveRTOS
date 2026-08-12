/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Engine-neutral raw block-media access. Filesystem policy and partition
 * interpretation deliberately live above this sector-oriented contract.
 *
 * Every transfer requires a caller-owned handle from @ref ove_block_open.
 * Read handles may coexist with the native filesystem; a write handle owns the
 * medium exclusively and therefore fails with @c OVE_ERR_BUSY while a
 * filesystem or another raw handle owns it. Handles are tied to the removable
 * medium generation and fail closed after card removal/replacement.
 */

#ifndef OVE_BLOCK_H
#define OVE_BLOCK_H

#include "ove/types.h"
#include "ove/media.h"
#include "ove_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVE_BLOCK_F_REMOVABLE 0x01u
#define OVE_BLOCK_F_READ_ONLY 0x02u
#define OVE_BLOCK_F_MEDIA_PRESENT 0x04u

#define OVE_BLOCK_OPEN_WRITE 0x01u

struct ove_block_info {
	uint64_t block_count;
	uint32_t logical_block_size;
	uint32_t erase_block_size; /**< Erase granularity in bytes. */
	uint32_t flags;
	uint32_t generation;
};

/** Caller-owned raw block handle carrying its media lease. */
typedef struct ove_block {
	struct ove_media_lease lease;
	uint8_t flags;
	uint8_t _reserved[3];
} ove_block_t;

/** Static/caller-stack initializer required before the first open. */
#define OVE_BLOCK_INITIALIZER {0}

#ifdef CONFIG_OVE_BLOCK
/** Query current geometry/presence without acquiring a transfer lease. */
int ove_block_get_info(struct ove_block_info *out);
/** Acquire a raw reader, or an exclusive writer with OVE_BLOCK_OPEN_WRITE. */
int ove_block_open(ove_block_t *block, unsigned flags);
/** Release a raw lease; a valid writer is flushed before ownership is dropped. */
void ove_block_close(ove_block_t *block);
int ove_block_read(ove_block_t *block, uint64_t first_block, uint32_t block_count, void *buffer);
int ove_block_write(ove_block_t *block, uint64_t first_block, uint32_t block_count,
		    const void *buffer);
int ove_block_sync(ove_block_t *block);
#else
static inline int ove_block_get_info(struct ove_block_info *out)
{
	(void)out;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_open(ove_block_t *block, unsigned flags)
{
	(void)block;
	(void)flags;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline void ove_block_close(ove_block_t *block)
{
	(void)block;
}
static inline int ove_block_read(ove_block_t *block, uint64_t first_block, uint32_t block_count,
				 void *buffer)
{
	(void)block;
	(void)first_block;
	(void)block_count;
	(void)buffer;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_write(ove_block_t *block, uint64_t first_block, uint32_t block_count,
				  const void *buffer)
{
	(void)block;
	(void)first_block;
	(void)block_count;
	(void)buffer;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_sync(ove_block_t *block)
{
	(void)block;
	return OVE_ERR_NOT_SUPPORTED;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVE_BLOCK_H */
