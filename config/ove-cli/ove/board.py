# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Board import/sync — thin wrapper around config/scripts/board_tool.py.

The board_tool.py is 1600+ lines and handles complex RTOS-specific import
logic. Rather than duplicating it, we delegate to it.
"""

import glob
import os
import subprocess
import sys

from .workspace import Workspace, find_ove_dir


def _find_rtos_base(ove_dir, ws, rtos):
    """Find RTOS source tree, searching workspace dl then shared dl/.

    Returns the path if found, or None.
    """
    if rtos == "zephyr":
        subpath = os.path.join("zephyr-workspace", "zephyr")
    elif rtos == "nuttx":
        subpath = rtos
    else:
        return None

    # 1. Active workspace dl/
    candidate = os.path.join(ws.ws_dl_dir, subpath)
    if os.path.isdir(candidate):
        return candidate

    # 2. Shared dl/ (may have a hash suffix, e.g. dl/zephyr-workspace-4720cee8/)
    dl_dir = os.path.join(ove_dir, "dl")
    if rtos == "zephyr":
        for match in sorted(glob.glob(os.path.join(dl_dir, "zephyr-workspace*",
                                                    "zephyr"))):
            if os.path.isdir(match):
                return match
    elif rtos == "nuttx":
        candidate = os.path.join(dl_dir, "nuttx")
        if os.path.isdir(candidate):
            return candidate

    # 3. Any other workspace dl/
    output_dir = os.path.join(ove_dir, "output")
    if os.path.isdir(output_dir):
        for match in sorted(glob.glob(os.path.join(output_dir, "**", "dl",
                                                    subpath))):
            if os.path.isdir(match):
                return match

    return None


def _board_tool(ove_dir, args):
    """Run config/scripts/board_tool.py with the given arguments."""
    script = os.path.join(ove_dir, "config", "scripts", "board_tool.py")
    python = os.path.join(ove_dir, ".venv", "bin", "python")
    if not os.path.isfile(python):
        python = sys.executable

    env = dict(os.environ)
    env["OVE_DIR"] = ove_dir

    cmd = [python, script] + args
    result = subprocess.run(cmd, env=env)
    if result.returncode != 0:
        sys.exit(result.returncode)


def cmd_board(args):
    """CLI entry point for 'ove board <subcommand>'."""
    ove_dir = find_ove_dir()

    if args.board_action == "list":
        _board_tool(ove_dir, ["list"])

    elif args.board_action == "import":
        if not args.rtos:
            print("Error: --rtos required for import (zephyr or nuttx)")
            sys.exit(1)
        if not args.name:
            print("Error: --name required for import")
            sys.exit(1)

        ws = Workspace(ove_dir)
        cmd = ["import", args.rtos, "--board", args.name]

        rtos_base = _find_rtos_base(ove_dir, ws, args.rtos)
        if rtos_base:
            if args.rtos == "zephyr":
                cmd.extend(["--zephyr-base", rtos_base])
            elif args.rtos == "nuttx":
                cmd.extend(["--nuttx-base", rtos_base])
        # If not found, board_tool.py will use its own default and error if missing

        if args.ove_name:
            cmd.extend(["--ove-name", args.ove_name])

        _board_tool(ove_dir, cmd)

    elif args.board_action == "register":
        if not args.name:
            print("Error: --name required for register")
            sys.exit(1)
        _board_tool(ove_dir, ["register", "--board", args.name])

    elif args.board_action == "sync":
        cmd = ["sync"]
        if args.name:
            cmd.extend(["--board", args.name])
        else:
            # Sync all boards
            boards_dir = os.path.join(ove_dir, "boards")
            for entry in sorted(os.listdir(boards_dir)):
                yaml_path = os.path.join(boards_dir, entry, "board.yaml")
                if os.path.isfile(yaml_path):
                    sync_cmd = ["sync", "--board", entry]
                    if args.rtos:
                        sync_cmd.extend(["--rtos", args.rtos])
                    else:
                        sync_cmd.extend(["--rtos", "all"])
                    print(f"Syncing {entry}...")
                    _board_tool(ove_dir, sync_cmd)
            return

        if args.rtos:
            cmd.extend(["--rtos", args.rtos])
        else:
            cmd.extend(["--rtos", "all"])
        _board_tool(ove_dir, cmd)

    else:
        print(f"Error: unknown board action '{args.board_action}'")
        sys.exit(1)
