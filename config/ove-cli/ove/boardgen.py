# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Generate the board selection Kconfig from boards/*/board.yaml.

config/Config.in.hardware sources output/kconfig/boards/Config.in, so like
the app Kconfig it must be generated before kconfiglib parses Config.in.
"""

import os

import yaml

# Config.in.rtos order.
RTOS_SYMBOLS = (
    ("freertos", "OVE_RTOS_FREERTOS"),
    ("zephyr", "OVE_RTOS_ZEPHYR"),
    ("nuttx", "OVE_RTOS_NUTTX"),
)


def load_boards(ove_dir):
    """Return ``(dir_name, board.yaml data)`` for every board, default first."""
    boards_dir = os.path.join(ove_dir, "boards")
    boards = []
    for entry in sorted(os.listdir(boards_dir)):
        path = os.path.join(boards_dir, entry, "board.yaml")
        if os.path.isfile(path):
            with open(path) as f:
                boards.append((entry, yaml.safe_load(f) or {}))
    boards.sort(key=lambda b: not b[1]["board"].get("default", False))
    return boards


def _help_lines(data):
    board = data["board"]
    memory = data.get("memory", {})
    facts = board["mcu"]
    if "sysclk_mhz" in data.get("clock", {}):
        facts += f" @ {data['clock']['sysclk_mhz']} MHz"
    if "flash_kb" in memory:
        facts += f", {memory['flash_kb']} KB flash"
    if "ram_kb" in memory:
        facts += f", {memory['ram_kb']} KB RAM"
    lines = board.get("description", "").strip().splitlines()
    lines.append(facts + ".")
    return lines


def board_kconfig(boards):
    """Return the Kconfig text for *boards* (from :func:`load_boards`)."""
    lines = [
        "# Generated from boards/*/board.yaml — do not edit.",
        "",
        "choice",
        '    prompt "Target board"',
        "",
    ]
    for _, data in boards:
        board = data["board"]
        rtoses = [sym for rtos, sym in RTOS_SYMBOLS if rtos in data["rtos"]]
        lines.append(f"config {board['symbol']}")
        lines.append(f'    bool "{board["title"]}"')
        lines.append("    depends on " + " || ".join(rtoses))
        for line in board.get("kconfig", "").strip().splitlines():
            lines.append("    " + line.strip())
        lines.append("    help")
        lines += [f"      {line}" if line else "" for line in _help_lines(data)]
        lines.append("")
    lines += ["endchoice", ""]

    def size(data, key):
        kb = data.get("memory", {}).get(key)
        return None if kb is None else kb * 1024

    derived = (
        ("OVE_BOARD_NAME", "string", lambda name, data: f'"{name}"'),
        ("OVE_FLASH_SIZE", "int", lambda name, data: size(data, "flash_kb")),
        ("OVE_RAM_SIZE", "int", lambda name, data: size(data, "ram_kb")),
        ("OVE_MCU", "string", lambda name, data: f'"{data["board"]["mcu"]}"'),
    )
    for symbol, kind, value in derived:
        lines += [f"config {symbol}", f"    {kind}"]
        for name, data in boards:
            # A board without the fact leaves the symbol at 0 / "".
            if value(name, data) is not None:
                lines.append(f"    default {value(name, data)} if {data['board']['symbol']}")
        lines.append("")
    return "\n".join(lines)


def generate_board_kconfig(ove_dir):
    """Write output/kconfig/boards/Config.in."""
    kconfig_dir = os.path.join(ove_dir, "output", "kconfig", "boards")
    os.makedirs(kconfig_dir, exist_ok=True)
    with open(os.path.join(kconfig_dir, "Config.in"), "w") as f:
        f.write(board_kconfig(load_boards(ove_dir)))


def board_symbol(ove_dir, board_dir_name):
    """Return the Kconfig symbol of a board directory, or None."""
    path = os.path.join(ove_dir, "boards", board_dir_name, "board.yaml")
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return ((yaml.safe_load(f) or {}).get("board") or {}).get("symbol")
