/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Engine-neutral raw block-media access. Filesystem policy and partition
 * interpretation deliberately live above this sector-oriented contract.
 */

#ifndef OVE_BLOCK_H
#define OVE_BLOCK_H

#include "ove/types.h"
#include "ove_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVE_BLOCK_F_REMOVABLE 0x01u
#define OVE_BLOCK_F_READ_ONLY 0x02u
#define OVE_BLOCK_F_MEDIA_PRESENT 0x04u

struct ove_block_info {
	uint64_t block_count;
	uint32_t logical_block_size;
	uint32_t erase_block_size; /**< Erase granularity in bytes. */
	uint32_t flags;
	uint32_t generation;
};

#ifdef CONFIG_OVE_BLOCK
int ove_block_get_info(struct ove_block_info *out);
int ove_block_read(uint64_t first_block, uint32_t block_count, void *buffer);
int ove_block_write(uint64_t first_block, uint32_t block_count, const void *buffer);
int ove_block_sync(void);
#else
static inline int ove_block_get_info(struct ove_block_info *out)
{
	(void)out;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_read(uint64_t first_block, uint32_t block_count, void *buffer)
{
	(void)first_block;
	(void)block_count;
	(void)buffer;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_write(uint64_t first_block, uint32_t block_count, const void *buffer)
{
	(void)first_block;
	(void)block_count;
	(void)buffer;
	return OVE_ERR_NOT_SUPPORTED;
}
static inline int ove_block_sync(void)
{
	return OVE_ERR_NOT_SUPPORTED;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OVE_BLOCK_H */
