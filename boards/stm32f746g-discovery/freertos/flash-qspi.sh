#!/bin/bash
# Program the Linux rootfs.cpio into the STM32F746G-Discovery on-board QSPI NOR
# (Micron N25Q128A, 16 MB) at 0x90000000.
#
# This uses OpenOCD's stm32f746g-disco board script, which enables the stmqspi
# flash driver and brings up QUADSPI directly through reset-init. No matching
# firmware image or SDRAM staging hook is required.
#
# Usage: flash-qspi.sh <rootfs.cpio>
set -euo pipefail

usage() {
	echo "Usage: $0 <rootfs.cpio>" >&2
	echo "Legacy two-argument form is accepted as: $0 <firmware.elf> <rootfs.cpio>" >&2
}

case "$#" in
1)
	CPIO=$1
	;;
2)
	echo "warning: firmware argument is ignored; using OpenOCD stmqspi direct programming" >&2
	CPIO=$2
	;;
*)
	usage
	exit 1
	;;
esac

[ -f "$CPIO" ] || { echo "not found: $CPIO" >&2; exit 1; }

OPENOCD=${OPENOCD:-openocd}
BOARD_CFG=${BOARD_CFG:-board/stm32f746g-disco.cfg}
QSPI_ADDR=${QSPI_ADDR:-0x90000000}
QSPI_BANK=${QSPI_BANK:-3}
MAX_QSPI=$((16 * 1024 * 1024))

command -v "$OPENOCD" >/dev/null 2>&1 || { echo "not found: $OPENOCD" >&2; exit 1; }

LEN=$(stat -c%s "$CPIO")
if [ "$LEN" -gt "$MAX_QSPI" ]; then
	echo "rootfs too large for QSPI: $LEN > $MAX_QSPI" >&2
	exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
CPIO_REAL=$(realpath "$CPIO")
CPIO_OPENOCD="$TMPDIR/rootfs.cpio"
ln -s "$CPIO_REAL" "$CPIO_OPENOCD"

echo "programming $LEN bytes -> QSPI $QSPI_ADDR (OpenOCD stmqspi bank $QSPI_BANK)"
echo "erase is padded to the QSPI sector boundary by OpenOCD"

"$OPENOCD" -f "$BOARD_CFG" \
	-c "init" \
	-c "reset init" \
	-c "flash probe $QSPI_BANK" \
	-c "flash erase_address pad $QSPI_ADDR $LEN" \
	-c "flash write_bank $QSPI_BANK $CPIO_OPENOCD 0" \
	-c "flash verify_bank $QSPI_BANK $CPIO_OPENOCD 0" \
	-c "reset run" \
	-c "exit"

echo "OK - rootfs programmed and verified in QSPI."
