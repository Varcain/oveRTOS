/* NuttX native block-driver backend for the STM32 SDMMC device. */
#include "ove/block.h"

#ifdef CONFIG_OVE_BLOCK

#include <nuttx/fs/fs.h>

#include <errno.h>
#include <string.h>
#include <sys/mount.h>

#define OVE_NUTTX_SD_DEVICE "/dev/mmcsd0"

static int with_device(int writable, struct inode **inode)
{
	int rc = open_blockdriver(OVE_NUTTX_SD_DEVICE, writable ? 0 : MS_RDONLY, inode);
	return rc < 0 ? (rc == -ENOENT ? OVE_ERR_NOT_REGISTERED : OVE_ERR_IO) : OVE_OK;
}

int ove_block_get_info(struct ove_block_info *out)
{
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	struct inode *inode = NULL;
	int rc = with_device(0, &inode);
	if (rc != OVE_OK)
		return rc;
	struct geometry geometry;
	memset(&geometry, 0, sizeof(geometry));
	int native = inode->u.i_bops->geometry(inode, &geometry);
	(void)close_blockdriver(inode);
	if (native < 0 || !geometry.geo_available)
		return OVE_ERR_NOT_REGISTERED;
	memset(out, 0, sizeof(*out));
	out->block_count = geometry.geo_nsectors;
	out->logical_block_size = geometry.geo_sectorsize;
	out->erase_block_size = geometry.geo_sectorsize;
	out->flags = OVE_BLOCK_F_REMOVABLE | OVE_BLOCK_F_MEDIA_PRESENT;
	if (!geometry.geo_writeenabled)
		out->flags |= OVE_BLOCK_F_READ_ONLY;
	out->generation = geometry.geo_mediachanged ? 2u : 1u;
	return OVE_OK;
}

static int transfer(uint64_t first_block, uint32_t block_count, void *buffer, int write)
{
	if (!buffer && block_count)
		return OVE_ERR_INVALID_PARAM;
	if (block_count == 0u)
		return OVE_OK;
	struct inode *inode = NULL;
	int rc = with_device(write, &inode);
	if (rc != OVE_OK)
		return rc;
	ssize_t done = write ? inode->u.i_bops->write(inode, buffer, (blkcnt_t)first_block,
						       block_count)
			     : inode->u.i_bops->read(inode, buffer, (blkcnt_t)first_block,
						      block_count);
	(void)close_blockdriver(inode);
	return done == (ssize_t)block_count ? OVE_OK : OVE_ERR_IO;
}

int ove_block_read(uint64_t first_block, uint32_t block_count, void *buffer)
{
	return transfer(first_block, block_count, buffer, 0);
}

int ove_block_write(uint64_t first_block, uint32_t block_count, const void *buffer)
{
	return transfer(first_block, block_count, (void *)buffer, 1);
}

int ove_block_sync(void)
{
	struct inode *inode = NULL;
	int rc = with_device(1, &inode);
	if (rc != OVE_OK)
		return rc;
	int native = inode->u.i_bops->ioctl
			     ? inode->u.i_bops->ioctl(inode, BIOC_FLUSH, 0u)
			     : 0;
	(void)close_blockdriver(inode);
	/* STM32 mmcsd completes writes synchronously and may not implement the
	 * optional flush ioctl. Absence of that ioctl is therefore already durable. */
	return native == -ENOTTY || native == -ENOSYS ? OVE_OK
	       : native < 0                         ? OVE_ERR_IO
					    : OVE_OK;
}

#endif /* CONFIG_OVE_BLOCK */
