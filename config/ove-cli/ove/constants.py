# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Centralized CLI constants."""

NUTTX_BOARD_CONFIGS = {
    "qemu-mps2-an500": "mps2-an500:nsh",
    "stm32f746g-discovery": "stm32f746g-disco:nsh",
}

ZEPHYR_BOARD_MAPPINGS = {
    "stm32f746g-discovery": "stm32f746g_disco",
    "qemu-mps2-an500": "mps2/an500",
    "qemu-mps2-an521": "mps2/an521/cpu0",
}
