# NuttX native SD/FAT substrate for CONFIG_OVE_FS. Keep this feature-conditional
# so applications that do not use storage do not pay for the controller or VFS.
CONFIG_STM32F7_SDMMC1=y
CONFIG_MMCSD=y
CONFIG_MMCSD_SDIO=y
CONFIG_FS_FAT=y
CONFIG_FAT_LFN=y
CONFIG_FAT_LFN_UTF8=y
CONFIG_FAT_MAXFNAME=255
CONFIG_NAME_MAX=255
CONFIG_PATH_MAX=320
