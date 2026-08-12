/* NuttX native block-driver backend for the STM32 SDMMC device. */
#include "ove/block_backend.h"

#ifdef CONFIG_OVE_BLOCK

#include <nuttx/fs/fs.h>

#include <errno.h>
#include <string.h>
#include <sys/mount.h>

#define OVE_NUTTX_SD_DEVICE "/dev/mmcsd0"

static int g_media_present = -1;
static uint32_t g_media_generation = 1u;

static void note_presence(int present)
{
	int old = __atomic_exchange_n(&g_media_present, present, __ATOMIC_ACQ_REL);
	if (old >= 0 && old != present)
		(void)__atomic_add_fetch(&g_media_generation, 1u, __ATOMIC_ACQ_REL);
}

static int with_device(int writable, struct inode **inode)
{
	int rc = open_blockdriver(OVE_NUTTX_SD_DEVICE, writable ? 0 : MS_RDONLY, inode);
	if (rc < 0) {
		if (rc == -ENOENT)
			note_presence(0);
		return rc == -ENOENT ? OVE_ERR_NOT_REGISTERED : OVE_ERR_IO;
	}
	return OVE_OK;
}

int ove_block_backend_get_info(struct ove_block_info *out)
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
	if (native < 0 || !geometry.geo_available) {
		note_presence(0);
		return OVE_ERR_NOT_REGISTERED;
	}
	note_presence(1);
	memset(out, 0, sizeof(*out));
	out->block_count = geometry.geo_nsectors;
	out->logical_block_size = geometry.geo_sectorsize;
	out->erase_block_size = geometry.geo_sectorsize;
	out->flags = OVE_BLOCK_F_REMOVABLE | OVE_BLOCK_F_MEDIA_PRESENT;
	if (!geometry.geo_writeenabled)
		out->flags |= OVE_BLOCK_F_READ_ONLY;
	out->generation = __atomic_load_n(&g_media_generation, __ATOMIC_ACQUIRE);
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
	ssize_t done =
		write ? inode->u.i_bops->write(inode, buffer, (blkcnt_t)first_block, block_count)
		      : inode->u.i_bops->read(inode, buffer, (blkcnt_t)first_block, block_count);
	(void)close_blockdriver(inode);
	return done == (ssize_t)block_count ? OVE_OK : OVE_ERR_IO;
}

int ove_block_backend_read(uint64_t first_block, uint32_t block_count, void *buffer)
{
	return transfer(first_block, block_count, buffer, 0);
}

int ove_block_backend_write(uint64_t first_block, uint32_t block_count, const void *buffer)
{
	return transfer(first_block, block_count, (void *)buffer, 1);
}

int ove_block_backend_sync(void)
{
	struct inode *inode = NULL;
	int rc = with_device(1, &inode);
	if (rc != OVE_OK)
		return rc;
	int native = inode->u.i_bops->ioctl ? inode->u.i_bops->ioctl(inode, BIOC_FLUSH, 0u) : 0;
	(void)close_blockdriver(inode);
	/* STM32 mmcsd completes writes synchronously and may not implement the
	 * optional flush ioctl. Absence of that ioctl is therefore already durable. */
	return native == -ENOTTY || native == -ENOSYS ? OVE_OK : native < 0 ? OVE_ERR_IO : OVE_OK;
}

#endif /* CONFIG_OVE_BLOCK */
