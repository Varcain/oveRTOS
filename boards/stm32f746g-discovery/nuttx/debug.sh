#!/bin/bash
# Start OpenOCD debug server for STM32F746G-Discovery (NuttX)
# Usage: debug.sh

set -e

openocd -f board/stm32f7discovery.cfg
