#!/bin/bash
# Flash script for STM32F746G-Discovery (FreeRTOS)
# Usage: flash.sh <firmware.elf>

set -e

FIRMWARE="${1:?Usage: $0 <firmware.elf>}"

if [ ! -f "$FIRMWARE" ]; then
    echo "Error: firmware file not found: $FIRMWARE"
    exit 1
fi

openocd \
    -f board/stm32f7discovery.cfg \
    -c "program $FIRMWARE verify reset exit"
