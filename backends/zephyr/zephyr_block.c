/* Zephyr disk-access SD block backend. */
#include "ove/block_backend.h"

#ifdef CONFIG_OVE_BLOCK

#include <zephyr/storage/disk_access.h>

#include <limits.h>
#include <string.h>

#define OVE_ZEPHYR_SD_DISK "SD"

static uint32_t g_sector_size;
static int g_media_present = -1;
static uint32_t g_media_generation = 1u;

static void note_presence(int present)
{
	int old = __atomic_exchange_n(&g_media_present, present, __ATOMIC_ACQ_REL);
	if (old >= 0 && old != present)
		(void)__atomic_add_fetch(&g_media_generation, 1u, __ATOMIC_ACQ_REL);
}

static int ensure_ready(void)
{
	int status = disk_access_status(OVE_ZEPHYR_SD_DISK);
	if ((status & DISK_STATUS_UNINIT) != 0 && disk_access_init(OVE_ZEPHYR_SD_DISK) != 0)
		return OVE_ERR_NOT_REGISTERED;
	status = disk_access_status(OVE_ZEPHYR_SD_DISK);
	if ((status & (DISK_STATUS_UNINIT | DISK_STATUS_NOMEDIA)) != 0) {
		g_sector_size = 0u;
		note_presence(0);
		return OVE_ERR_NOT_REGISTERED;
	}
	note_presence(1);
	return OVE_OK;
}

static int get_sector_size(uint32_t *out)
{
	if (g_sector_size == 0u &&
	    disk_access_ioctl(OVE_ZEPHYR_SD_DISK, DISK_IOCTL_GET_SECTOR_SIZE, &g_sector_size) != 0)
		return OVE_ERR_IO;
	if (g_sector_size == 0u)
		return OVE_ERR_IO;
	*out = g_sector_size;
	return OVE_OK;
}

int ove_block_backend_get_info(struct ove_block_info *out)
{
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	uint32_t sectors = 0;
	uint32_t sector_size = 0;
	uint32_t erase_sectors = 1;
	if (disk_access_ioctl(OVE_ZEPHYR_SD_DISK, DISK_IOCTL_GET_SECTOR_COUNT, &sectors) != 0 ||
	    get_sector_size(&sector_size) != OVE_OK)
		return OVE_ERR_IO;
	if (disk_access_ioctl(OVE_ZEPHYR_SD_DISK, DISK_IOCTL_GET_ERASE_BLOCK_SZ, &erase_sectors) !=
		    0 ||
	    erase_sectors == 0u)
		erase_sectors = 1u;
	memset(out, 0, sizeof(*out));
	out->block_count = sectors;
	out->logical_block_size = sector_size;
	out->erase_block_size =
		erase_sectors > UINT32_MAX / sector_size ? UINT32_MAX : erase_sectors * sector_size;
	out->flags = OVE_BLOCK_F_REMOVABLE | OVE_BLOCK_F_MEDIA_PRESENT;
	if ((disk_access_status(OVE_ZEPHYR_SD_DISK) & DISK_STATUS_WR_PROTECT) != 0)
		out->flags |= OVE_BLOCK_F_READ_ONLY;
	out->generation = __atomic_load_n(&g_media_generation, __ATOMIC_ACQUIRE);
	return OVE_OK;
}

int ove_block_backend_read(uint64_t first_block, uint32_t block_count, void *buffer)
{
	if ((!buffer && block_count) || first_block > UINT32_MAX ||
	    (uint64_t)block_count > (uint64_t)UINT32_MAX + 1u - first_block)
		return OVE_ERR_INVALID_PARAM;
	if (block_count == 0u)
		return OVE_OK;
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	uint32_t size;
	if (get_sector_size(&size) != OVE_OK)
		return OVE_ERR_IO;
	/* CONFIG_SDMMC_STM32_SINGLE_BLOCK keeps each native request on the stable
	 * STM32 path even when the personality submits a bounded multi-sector span. */
	uint8_t *bytes = buffer;
	for (uint32_t i = 0; i < block_count; i++)
		if (disk_access_read(OVE_ZEPHYR_SD_DISK, bytes + (size_t)i * size,
				     (uint32_t)first_block + i, 1u) != 0)
			return OVE_ERR_IO;
	return OVE_OK;
}

int ove_block_backend_write(uint64_t first_block, uint32_t block_count, const void *buffer)
{
	if ((!buffer && block_count) || first_block > UINT32_MAX ||
	    (uint64_t)block_count > (uint64_t)UINT32_MAX + 1u - first_block)
		return OVE_ERR_INVALID_PARAM;
	if (block_count == 0u)
		return OVE_OK;
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	uint32_t size;
	if (get_sector_size(&size) != OVE_OK)
		return OVE_ERR_IO;
	const uint8_t *bytes = buffer;
	for (uint32_t i = 0; i < block_count; i++)
		if (disk_access_write(OVE_ZEPHYR_SD_DISK, bytes + (size_t)i * size,
				      (uint32_t)first_block + i, 1u) != 0)
			return OVE_ERR_IO;
	return OVE_OK;
}

int ove_block_backend_sync(void)
{
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	return disk_access_ioctl(OVE_ZEPHYR_SD_DISK, DISK_IOCTL_CTRL_SYNC, NULL) == 0 ? OVE_OK
										      : OVE_ERR_IO;
}

#endif /* CONFIG_OVE_BLOCK */
