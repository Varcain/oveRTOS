# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Centralized CLI constants."""

NUTTX_DEFAULT_TAG = "nuttx-12.12.0"
ZEPHYR_DEFAULT_REV = "caa8079a5362cd0437ec4d74c888077857df1a9c"
ARM_TOOLCHAIN_URL = (
    "https://developer.arm.com/-/media/Files/downloads/gnu/"
    "13.3.rel1/binrel/"
    "arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz"
)

NUTTX_BOARD_CONFIGS = {
    "qemu-mps2-an500": "mps2-an500:nsh",
    "stm32f746g-discovery": "stm32f746g-disco:nsh",
}

ZEPHYR_BOARD_MAPPINGS = {
    "stm32f746g-discovery": "stm32f746g_disco",
    "qemu-mps2-an500": "mps2/an500",
}
