#!/bin/bash
# Program the Linux rootfs.cpio into the STM32F746-Discovery on-board QSPI NOR
# (N25Q128A, 16 MB) at 0x90000000, so a CONFIG_OVE_LINUX_ROOTFS_QSPI firmware
# XIPs its rootfs from external flash (the internal 1 MB flash then holds only
# the firmware, so a large LVGL rootfs fits).
#
# Firmware-assisted: the QSPI firmware must already be flashed (flash.sh).  We
# halt at bsp_qspi_flash_stage (SDRAM + QUADSPI-indirect up), stage the cpio in
# SDRAM + a {magic,len} request header, and let the target erase + program the
# NOR with the ST-validated BSP_QSPI_Write at QUADSPI speed — far faster + more
# robust than programming over SWD, and it reuses the firmware's own QSPI
# bring-up.  bsp_qspi_flash_stage sets the header to 'DONE' when it finishes;
# BSP_QSPI_EnableMemoryMappedMode runs immediately after it, so a breakpoint
# there is exactly "programming complete" (the 4 KB-granular erase of the whole
# rootfs takes tens of seconds — do NOT use a fixed delay).
#
# Usage: flash-qspi.sh <firmware.elf> <rootfs.cpio>
set -e

FIRMWARE="${1:?Usage: $0 <firmware.elf> <rootfs.cpio>}"
CPIO="${2:?Usage: $0 <firmware.elf> <rootfs.cpio>}"
[ -f "$FIRMWARE" ] || { echo "not found: $FIRMWARE"; exit 1; }
[ -f "$CPIO" ] || { echo "not found: $CPIO"; exit 1; }

# SDRAM staging (transient — before the personality's SDRAM pools are used):
#   header {magic, len} at 0xC01F0000, cpio at 0xC0200000.  Kept in step with bsp.c.
HDR=0xC01F0000
DATA=0xC0200000
REQ=0x51535052  # 'QSPR' — request a program
DONE=0x444f4e45 # 'DONE' — target finished

NM=$(command -v arm-none-eabi-nm || echo arm-none-eabi-nm)
STAGE=$("$NM" "$FIRMWARE" | awk '/ bsp_qspi_flash_stage$/{print "0x"$1}')
MMAP=$("$NM" "$FIRMWARE" | awk '/ BSP_QSPI_EnableMemoryMappedMode$/{print "0x"$1}')
[ -n "$STAGE" ] || { echo "bsp_qspi_flash_stage symbol not found in $FIRMWARE"; exit 1; }
[ -n "$MMAP" ] || { echo "BSP_QSPI_EnableMemoryMappedMode symbol not found in $FIRMWARE"; exit 1; }
LEN=$(stat -c%s "$CPIO")
echo "staging $LEN bytes -> QSPI (bp bsp_qspi_flash_stage @ $STAGE, done @ $MMAP)"

OUT=$(openocd -f interface/stlink.cfg -f target/stm32f7x.cfg \
    -c "init" \
    -c "reset halt" \
    -c "bp $STAGE 2 hw" \
    -c "bp $MMAP 2 hw" \
    -c "resume" \
    -c "wait_halt 10000" \
    -c "load_image $CPIO $DATA bin" \
    -c "mww [expr {$HDR + 4}] $LEN" \
    -c "mww $HDR $REQ" \
    -c "resume" \
    -c "wait_halt 180000" \
    -c "mdw $HDR 1" \
    -c "rbp $STAGE" \
    -c "rbp $MMAP" \
    -c "reset run" \
    -c "exit" 2>&1)
echo "$OUT"

# The mdw of the header must read back 'DONE' (0x444f4e45); anything else means
# the erase/program did not complete (e.g. the wait timed out).
if echo "$OUT" | grep -qi "${DONE#0x}"; then
    echo "OK — rootfs programmed; the target now boots from the QSPI rootfs."
else
    echo "FAILED — header did not read back DONE; QSPI may be partially programmed."
    exit 1
fi
