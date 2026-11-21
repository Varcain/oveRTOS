# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of oveRTOS.

import contextlib
import io
import re
import runpy
import unittest
from pathlib import Path
from unittest.mock import ANY, patch

from ove import cli, completion, workspace


class BoardCommandsTest(unittest.TestCase):
    def test_completion_only_offers_current_board_commands(self):
        for shell in ("bash", "zsh", "fish"):
            with self.subTest(shell=shell):
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    completion._emit(shell)
                commands = re.search(r"^.*\blist\b.*\bimport\b.*$",
                                     output.getvalue(), re.MULTILINE)
                self.assertIsNotNone(commands)
                self.assertNotRegex(commands.group(), r"\b(?:register|sync)\b")

    def test_import_forwards_options(self):
        argv = ["ove", "board", "import", "--rtos", "nuttx", "--name", "demo",
                "--config", "netnsh", "--non-interactive", "--overwrite"]
        with patch("sys.argv", argv), \
                patch("ove.board._find_rtos_base", return_value=None), \
                patch("ove.board._board_tool") as run:
            cli.main()
        run.assert_called_once_with(
            ANY, ["import", "nuttx", "--board", "demo", "--config", "netnsh",
                  "--non-interactive", "--overwrite"])

    def test_zephyr_import_forwards_only_required_arguments_by_default(self):
        argv = ["ove", "board", "import", "--rtos", "zephyr", "--name", "demo"]
        with patch("sys.argv", argv), \
                patch("ove.board._find_rtos_base", return_value=None), \
                patch("ove.board._board_tool") as run:
            cli.main()
        run.assert_called_once_with(ANY, ["import", "zephyr", "--board", "demo"])

    def test_board_tool_uses_workspace_config_parser(self):
        root = Path(__file__).resolve().parents[2]
        tool = runpy.run_path(str(root / "config/scripts/board_tool.py"))
        self.assertIs(tool["parse_dotconfig"], workspace.parse_dotconfig)
