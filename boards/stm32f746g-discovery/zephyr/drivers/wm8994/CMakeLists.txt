# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_AUDIO_CODEC_WM8994 wm8994.c)
