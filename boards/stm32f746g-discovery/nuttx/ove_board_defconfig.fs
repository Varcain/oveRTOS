# NuttX native SD/FAT substrate for CONFIG_OVE_FS. Keep this feature-conditional
# so applications that do not use storage do not pay for the controller or VFS.
CONFIG_STM32F7_SDMMC1=y
CONFIG_STM32F7_DMA2=y
CONFIG_STM32F7_SDMMC_DMA=y
CONFIG_STM32F7_DMACAPABLE=y
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
# STM32F746 SDMMC multi-block DMA can intermittently end with both transfer
# completion and timeout asserted under concurrent LTDC/FMC traffic.  NuttX
# then reports EIO after a partially applied FAT metadata update.  Bound each
# transfer to one 4 KiB database page while retaining efficient CMD18/CMD25
# transfers; single-block mode makes VACUUM prohibitively slow on this card.
CONFIG_MMCSD_MULTIBLOCK_LIMIT=8
CONFIG_FS_FAT=y
CONFIG_FAT_DMAMEMORY=y
CONFIG_FAT_DIRECT_RETRY=y
CONFIG_FAT_LFN=y
CONFIG_FAT_LFN_UTF8=y
CONFIG_FAT_MAXFNAME=255
CONFIG_NAME_MAX=255
CONFIG_PATH_MAX=320
