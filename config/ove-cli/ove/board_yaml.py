# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""The engines a board supports and the upstream board each one builds.

Both come from the ``rtos:`` section of ``boards/<board>/board.yaml``::

    rtos:
      freertos: {}
      nuttx:  { board: stm32f746g-disco, config: nsh }
      zephyr: { board: stm32f746g_disco }
"""

import os

import yaml


def rtos_entry(board_dir, rtos):
    """Return the board's ``rtos.<rtos>`` mapping, or None if it lacks *rtos*."""
    path = os.path.join(board_dir, "board.yaml")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        data = yaml.safe_load(f) or {}
    entry = (data.get("rtos") or {}).get(rtos)
    if entry is None:
        return None
    return entry or {}


def nuttx_board_config(board_dir):
    """Return the NuttX ``<board>:<config>`` for a board, or None."""
    entry = rtos_entry(board_dir, "nuttx")
    if not entry:
        return None
    return f"{entry['board']}:{entry['config']}"


def zephyr_board(board_dir):
    """Return the Zephyr board identifier (``-b`` argument), or None."""
    entry = rtos_entry(board_dir, "zephyr")
    return entry["board"] if entry else None
