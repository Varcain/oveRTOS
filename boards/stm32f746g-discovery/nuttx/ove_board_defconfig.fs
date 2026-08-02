# NuttX native SD/FAT substrate for CONFIG_OVE_FS. Keep this feature-conditional
# so applications that do not use storage do not pay for the controller or VFS.
CONFIG_STM32F7_SDMMC1=y
CONFIG_STM32F7_DMA2=y
CONFIG_STM32F7_SDMMC_DMA=y
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
# The STM32F7 SDMMC DMA path supports multi-block transfers.  Keep NuttX's
# unlimited default explicit so an incrementally configured output tree cannot
# retain the single-block diagnostic workaround.
CONFIG_MMCSD_MULTIBLOCK_LIMIT=0
CONFIG_FS_FAT=y
CONFIG_FAT_DMAMEMORY=y
CONFIG_FAT_LFN=y
CONFIG_FAT_LFN_UTF8=y
CONFIG_FAT_MAXFNAME=255
CONFIG_NAME_MAX=255
CONFIG_PATH_MAX=320
