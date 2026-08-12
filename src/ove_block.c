/* Lease-bearing public raw block API over the engine backend. */
#include "ove/block.h"

#ifdef CONFIG_OVE_BLOCK

#include "ove/block_backend.h"
#include "ove/media.h"

#include <string.h>

static void refresh_presence_after_error(void)
{
	struct ove_block_info ignored;
	(void)ove_block_get_info(&ignored);
}

int ove_block_get_info(struct ove_block_info *out)
{
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	int rc = ove_block_backend_get_info(out);
	if (rc == OVE_OK) {
		int present = (out->flags & OVE_BLOCK_F_MEDIA_PRESENT) != 0u;
		ove_media_observe(out->generation, present);
	} else if (rc == OVE_ERR_NOT_REGISTERED) {
		/* Preserve the last generation while making every outstanding lease
		 * unusable. A backend advances its generation on reinsertion. */
		ove_media_removed();
	}
	return rc;
}

int ove_block_open(ove_block_t *block, unsigned flags)
{
	if (!block || block->lease.active || (flags & ~OVE_BLOCK_OPEN_WRITE) != 0u)
		return OVE_ERR_INVALID_PARAM;
	struct ove_block_info info;
	int rc = ove_block_get_info(&info);
	if (rc != OVE_OK)
		return rc;
	if ((flags & OVE_BLOCK_OPEN_WRITE) != 0u && (info.flags & OVE_BLOCK_F_READ_ONLY) != 0u)
		return OVE_ERR_READ_ONLY;
	rc = ove_media_raw_acquire(&block->lease,
				   (flags & OVE_BLOCK_OPEN_WRITE) ? OVE_MEDIA_RAW_WRITE : 0u,
				   info.generation);
	if (rc == OVE_OK)
		block->flags = (uint8_t)flags;
	return rc;
}

void ove_block_close(ove_block_t *block)
{
	if (!block)
		return;
	if ((block->flags & OVE_BLOCK_OPEN_WRITE) != 0u &&
	    ove_media_raw_valid(&block->lease, OVE_MEDIA_RAW_WRITE))
		(void)ove_block_backend_sync();
	ove_media_raw_release(&block->lease);
	block->flags = 0u;
}

int ove_block_read(ove_block_t *block, uint64_t first_block, uint32_t block_count, void *buffer)
{
	if (!block || !ove_media_raw_valid(&block->lease, 0u))
		return OVE_ERR_NOT_REGISTERED;
	int rc = ove_block_backend_read(first_block, block_count, buffer);
	if (rc != OVE_OK)
		refresh_presence_after_error();
	return rc;
}

int ove_block_write(ove_block_t *block, uint64_t first_block, uint32_t block_count,
		    const void *buffer)
{
	if (!block || !ove_media_raw_valid(&block->lease, 0u))
		return OVE_ERR_NOT_REGISTERED;
	if (!ove_media_raw_valid(&block->lease, OVE_MEDIA_RAW_WRITE))
		return OVE_ERR_PERMISSION;
	int rc = ove_block_backend_write(first_block, block_count, buffer);
	if (rc != OVE_OK)
		refresh_presence_after_error();
	return rc;
}

int ove_block_sync(ove_block_t *block)
{
	if (!block || !ove_media_raw_valid(&block->lease, 0u))
		return OVE_ERR_NOT_REGISTERED;
	int rc = ove_block_backend_sync();
	if (rc != OVE_OK)
		refresh_presence_after_error();
	return rc;
}

#endif /* CONFIG_OVE_BLOCK */
