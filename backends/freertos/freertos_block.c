/* FreeRTOS STM32 SD/FatFs diskio block backend. */
#include "ove/block_backend.h"

#ifdef CONFIG_OVE_BLOCK

#include "ff_gen_drv.h"
#include "sd_diskio.h"

#include <limits.h>

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
	if ((SD_Driver.disk_status(0) & STA_NOINIT) != 0u &&
	    (SD_Driver.disk_initialize(0) & STA_NOINIT) != 0u) {
		note_presence(0);
		return OVE_ERR_NOT_REGISTERED;
	}
	note_presence(1);
	return OVE_OK;
}

int ove_block_backend_get_info(struct ove_block_info *out)
{
	if (!out)
		return OVE_ERR_INVALID_PARAM;
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	DWORD sectors = 0;
	WORD sector_size = 0;
	DWORD erase_sectors = 1;
	if (SD_Driver.disk_ioctl(0, GET_SECTOR_COUNT, &sectors) != RES_OK ||
	    SD_Driver.disk_ioctl(0, GET_SECTOR_SIZE, &sector_size) != RES_OK)
		return OVE_ERR_IO;
	if (SD_Driver.disk_ioctl(0, GET_BLOCK_SIZE, &erase_sectors) != RES_OK ||
	    erase_sectors == 0u)
		erase_sectors = 1u;
	out->block_count = sectors;
	out->logical_block_size = sector_size;
	out->erase_block_size =
		erase_sectors > UINT32_MAX / sector_size ? UINT32_MAX : erase_sectors * sector_size;
	out->flags = OVE_BLOCK_F_REMOVABLE | OVE_BLOCK_F_MEDIA_PRESENT;
	if ((SD_Driver.disk_status(0) & STA_PROTECT) != 0u)
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
	return SD_Driver.disk_read(0, buffer, (DWORD)first_block, (UINT)block_count) == RES_OK
		       ? OVE_OK
		       : OVE_ERR_IO;
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
#if _USE_WRITE == 1
	return SD_Driver.disk_write(0, buffer, (DWORD)first_block, (UINT)block_count) == RES_OK
		       ? OVE_OK
		       : OVE_ERR_IO;
#else
	return OVE_ERR_READ_ONLY;
#endif
}

int ove_block_backend_sync(void)
{
	if (ensure_ready() != OVE_OK)
		return OVE_ERR_NOT_REGISTERED;
	return SD_Driver.disk_ioctl(0, CTRL_SYNC, NULL) == RES_OK ? OVE_OK : OVE_ERR_IO;
}

#endif /* CONFIG_OVE_BLOCK */
