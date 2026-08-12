/* Private backend side of the public, lease-bearing block API. */
#ifndef OVE_BLOCK_BACKEND_H
#define OVE_BLOCK_BACKEND_H

#include "ove/block.h"

int ove_block_backend_get_info(struct ove_block_info *out);
int ove_block_backend_read(uint64_t first_block, uint32_t block_count, void *buffer);
int ove_block_backend_write(uint64_t first_block, uint32_t block_count, const void *buffer);
int ove_block_backend_sync(void);

#endif /* OVE_BLOCK_BACKEND_H */
