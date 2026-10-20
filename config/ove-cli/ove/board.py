# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

"""Board listing and import via config/scripts/board_tool.py."""

import glob
import os
import subprocess
import sys

from . import venv
from .workspace import Workspace, find_ove_dir


def _find_nuttx_base(ove_dir, ws):
    """Find the NuttX source tree, searching workspace dl then shared dl/.

    Returns the path if found, or None.
    """
    # 1. Active workspace dl/
    candidate = os.path.join(ws.ws_dl_dir, "nuttx")
    if os.path.isdir(candidate):
        return candidate

    # 2. Shared dl/
    candidate = os.path.join(ove_dir, "dl", "nuttx")
    if os.path.isdir(candidate):
        return candidate

    # 3. Any other workspace dl/
    output_dir = os.path.join(ove_dir, "output")
    if os.path.isdir(output_dir):
        for match in sorted(glob.glob(os.path.join(output_dir, "**", "dl", "nuttx"))):
            if os.path.isdir(match):
                return match

    return None


def _board_tool(ove_dir, args):
    """Run config/scripts/board_tool.py with the given arguments."""
    script = os.path.join(ove_dir, "config", "scripts", "board_tool.py")
    python = venv.python_path(ove_dir)
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

        cmd = ["import", args.rtos, "--board", args.name]

        if args.rtos == "nuttx":
            rtos_base = _find_nuttx_base(ove_dir, Workspace(ove_dir))
            if rtos_base:
                cmd.extend(["--nuttx-base", rtos_base])

        if args.ove_name:
            cmd.extend(["--ove-name", args.ove_name])
        if args.config is not None:
            cmd.extend(["--config", args.config])
        if args.non_interactive:
            cmd.append("--non-interactive")
        if args.overwrite:
            cmd.append("--overwrite")

        _board_tool(ove_dir, cmd)

    else:
        print(f"Error: unknown board action '{args.board_action}'")
        sys.exit(1)
