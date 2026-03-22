#!/bin/bash
# Connect GDB to OpenOCD for STM32F746G-Discovery (NuttX)
# Usage: gdb.sh <firmware.elf>

set -e

FIRMWARE="${1:?Usage: $0 <firmware.elf>}"

if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware file not found: $FIRMWARE"
    exit 1
fi

arm-none-eabi-gdb "$FIRMWARE" \
    -ex "target extended-remote :3333" \
    -ex "monitor reset halt" \
    -ex "load"
